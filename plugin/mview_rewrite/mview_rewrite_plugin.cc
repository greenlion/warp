/* Copyright (c) 2026, Justin Swanhart and contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file plugin/mview_rewrite/mview_rewrite_plugin.cc

  The plugin MVIEW_REWRITE: a post-parse query rewrite plugin (an audit
  plugin of the parse class) that answers SELECT statements from
  materialized views.  See mview_rewrite.h for the algorithm.

  The registry: the definitions of the views are read from the data
  dictionary (mview_load_all()) and analysed in a thread of the plugin
  with its own session, whenever mview_registry_version has changed.  A
  statement waits for the load when that cannot block other sessions (it
  holds no metadata locks); otherwise it is not rewritten while the
  registry is out of date.  A registry that is out of date is never used:
  a view could have been dropped or created again with another definition.

  Which views may answer a statement: a view with REFRESH SYNCHRONOUS if
  the WARP_MVIEW daemon keeps it current (mview_is_maintained()); it
  trails the commits of its base tables by the delay of the daemon.  A
  view with REFRESH MANUAL holds the data of its last refresh: it is used
  only when the session sets mview_rewrite_use_manual_views.
*/

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <mysql/components/services/log_builtins.h>
#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>
#include <mysql/service_parser.h>

#include "my_inttypes.h"
#include "my_sys.h"
#include "my_thread.h"
#include "mysql/strings/m_ctype.h"
#include "mysqld_error.h"
#include "plugin/mview_rewrite/mview_rewrite.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/mview.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/protocol_classic.h"
#include "sql/replication.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/system_variables.h"
#include "sql/transaction.h"
#include "sql/transaction_info.h"

static SERVICE_TYPE(registry) *reg_srv = nullptr;
SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

namespace {

using mv_rewrite::Registry;

/* ------------------------------------------------------------------ */
/* Variables                                                           */
/* ------------------------------------------------------------------ */

MYSQL_THDVAR_BOOL(enabled, PLUGIN_VAR_OPCMDARG,
                  "Answer SELECT statements from materialized views.",
                  nullptr, nullptr, true);
MYSQL_THDVAR_BOOL(use_manual_views, PLUGIN_VAR_OPCMDARG,
                  "Also use materialized views with REFRESH MANUAL, which "
                  "hold the data of their last refresh.",
                  nullptr, nullptr, false);
MYSQL_THDVAR_BOOL(use_foreign_keys, PLUGIN_VAR_OPCMDARG,
                  "Use views with tables the query does not have, when "
                  "they are joined by foreign keys to unique keys.",
                  nullptr, nullptr, true);
MYSQL_THDVAR_BOOL(use_filter_tree, PLUGIN_VAR_OPCMDARG,
                  "Find the candidate views with the filter tree; OFF tests "
                  "every view.",
                  nullptr, nullptr, true);
MYSQL_THDVAR_BOOL(partial, PLUGIN_VAR_OPCMDARG,
                  "Let a view replace some of the tables of a query, joined "
                  "to the others.",
                  nullptr, nullptr, true);
MYSQL_THDVAR_BOOL(trace, PLUGIN_VAR_OPCMDARG,
                  "Explain in notes why each view is or is not used.",
                  nullptr, nullptr, false);

SYS_VAR *system_variables[] = {
    MYSQL_SYSVAR(enabled),          MYSQL_SYSVAR(use_manual_views),
    MYSQL_SYSVAR(use_foreign_keys), MYSQL_SYSVAR(use_filter_tree),
    MYSQL_SYSVAR(partial),          MYSQL_SYSVAR(trace),
    nullptr};

std::atomic<unsigned long long> st_statements{0};
std::atomic<unsigned long long> st_rewritten{0};
std::atomic<unsigned long long> st_candidates{0};
std::atomic<unsigned long long> st_loads{0};
std::atomic<unsigned long long> st_views{0};

SHOW_VAR status_variables[] = {
    {"Mview_rewrite_statements", pointer_cast<char *>(&st_statements),
     SHOW_LONGLONG, SHOW_SCOPE_GLOBAL},
    {"Mview_rewrite_rewritten", pointer_cast<char *>(&st_rewritten),
     SHOW_LONGLONG, SHOW_SCOPE_GLOBAL},
    {"Mview_rewrite_candidates", pointer_cast<char *>(&st_candidates),
     SHOW_LONGLONG, SHOW_SCOPE_GLOBAL},
    {"Mview_rewrite_registry_loads", pointer_cast<char *>(&st_loads),
     SHOW_LONGLONG, SHOW_SCOPE_GLOBAL},
    {"Mview_rewrite_views", pointer_cast<char *>(&st_views), SHOW_LONGLONG,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

void log_message(enum loglevel level, const std::string &message) {
  if (log_bi != nullptr)
    LogPluginErr(level, ER_LOG_PRINTF_MSG,
                 ("mview_rewrite: " + message).c_str());
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

std::mutex registry_mutex;
std::condition_variable registry_cond;
std::shared_ptr<const Registry> current_registry;
bool loading = false;
bool loader_joinable = false;
bool stopping = false;
my_thread_handle loader_thread;

int swallow_condition(int, const char *, const char *, void *) { return 1; }

/** The statement that computes the query block a view is made of. */
std::string definition_sql(const Mview_def &def) {
  std::string sql;
  if (!def.is_aggregate) {
    sql = def.detail_select + " " + def.from_sql;
    if (!def.where_sql.empty()) sql += " WHERE " + def.where_sql;
    return sql;
  }
  std::string groups;
  for (size_t i = 0; i < def.group_exprs.size(); i++)
    groups += (i ? ", " : "") + def.group_exprs[i];
  sql = "SELECT " + groups + (groups.empty() ? "" : ", ") + "count(*)";
  for (const Mview_agg_state &state : def.states) sql += ", " + state.full_sql;
  sql += " " + def.from_sql;
  if (!def.where_sql.empty()) sql += " WHERE " + def.where_sql;
  if (!groups.empty()) sql += " GROUP BY " + groups;
  return sql;
}

/** Analyse one view in the loader session. */
bool analyse_view(THD *thd, const Mview_def &def,
                  mv_rewrite::Catalog *catalog, Registry *registry,
                  mv_rewrite::View *view, std::string *why) {
  using namespace mv_rewrite;
  view->db = def.db;
  view->name = def.name;
  view->engine = def.engine;
  view->epoch = def.epoch;
  view->refresh_mode = def.refresh_mode;
  view->is_aggregate = def.is_aggregate;
  for (const Mview_table &t : def.tables) {
    if (t.null_supplying) {
      *why = "outer joins";
      return true;
    }
  }

  const std::string sql = definition_sql(def);
  thd->variables.sql_mode = 0;
  MYSQL_LEX_STRING text = {const_cast<char *>(sql.c_str()), sql.size()};
  if (mysql_parser_parse(thd, text, false, swallow_condition, nullptr) != 0 ||
      thd->lex->sql_command != SQLCOM_SELECT) {
    *why = "the definition cannot be parsed";
    return true;
  }
  if (make_block(thd, thd->lex->query_block, catalog, true, &view->block,
                 why))
    return true;

  /* what the select items are in the view table */
  if (!def.is_aggregate) {
    for (const Select_item &s : view->block.select) {
      View_output o;
      o.kind = View_output::PLAIN;
      o.column = s.name;
      view->outputs.push_back(o);
    }
  } else {
    const size_t groups = def.group_exprs.size();
    if (view->block.select.size() != groups + 1 + def.states.size()) {
      *why = "the definition does not have the expected columns";
      return true;
    }
    for (size_t i = 0; i < groups; i++) {
      View_output o;
      o.kind = View_output::GROUP;
      o.column = def.view_group_columns[i];
      view->outputs.push_back(o);
    }
    View_output count;
    count.kind = View_output::COUNT_STAR;
    count.column = def.count_column;
    view->outputs.push_back(count);
    for (size_t i = 0; i < def.states.size(); i++) {
      View_output o;
      o.kind = View_output::STATE;
      o.column = def.states[i].column;
      o.state = static_cast<int>(i);
      view->outputs.push_back(o);
      view->state_kinds.push_back(def.states[i].kind);
      const int c = def.states[i].count_state;
      view->state_count.push_back(
          c < 0 ? -1 : static_cast<int>(groups + 1 + c));
    }
  }

  std::vector<std::string> residuals;
  if (analyse_view_tables(thd, view, catalog, &residuals, why)) return true;
  for (size_t i = 0; i < view->block.insts.size(); i++) {
    const Instance &in = view->block.insts[i];
    const int id = registry->intern(in.db + "." + in.table);
    view->source_key.push_back(id);
    if (view->in_hub[i]) view->hub_key.push_back(id);
  }
  for (const std::string &sig : residuals)
    view->residual_key.push_back(registry->intern(sig));
  for (auto *key : {&view->source_key, &view->hub_key, &view->residual_key}) {
    std::sort(key->begin(), key->end());
    key->erase(std::unique(key->begin(), key->end()), key->end());
  }
  return false;
}

THD *make_session(const void *stack_base) {
  THD *thd = new (std::nothrow) THD;
  if (thd == nullptr) return nullptr;
  thd->set_new_thread_id();
  thd->capture_stack_base(stack_base);
  thd->store_globals();
  thd->set_command(COM_DAEMON);
  thd->system_thread = SYSTEM_THREAD_BACKGROUND;
  thd->get_protocol_classic()->set_client_capabilities(0);
  thd->security_context()->skip_grants();
  thd->get_protocol_classic()->init_net(nullptr);
  thd->init_query_mem_roots();
  lex_start(thd);
  thd->real_id = my_thread_self();
  thd->set_time();
  /* never wait long for a table: a statement may be waiting for us */
  thd->variables.lock_wait_timeout = 5;
  Global_THD_manager::get_instance()->add_thd(thd);
  return thd;
}

void destroy_session(THD *thd) {
  thd->get_protocol_classic()->end_net();
  thd->release_resources();
  Global_THD_manager::get_instance()->remove_thd(thd);
  delete thd;
}

/** Load and analyse every view; nullptr on failure. */
std::shared_ptr<Registry> load_registry(THD *thd) {
  auto registry = std::make_shared<Registry>();
  registry->version = mview_registry_version.load();
  registry->collation = thd->variables.collation_connection;

  std::vector<Mview_def> defs;
  thd->reset_for_next_command();
  thd->set_query_id(next_query_id());
  const bool failed = mview_load_all(thd, &defs);
  if (failed)
    trans_rollback_stmt(thd);
  else
    trans_commit_stmt(thd);
  close_thread_tables(thd);
  if (failed) {
    log_message(WARNING_LEVEL,
                std::string("cannot load the materialized views: ") +
                    (thd->is_error() ? thd->get_stmt_da()->message_text()
                                     : "unknown error"));
    thd->clear_error();
    trans_rollback(thd);
    thd->mdl_context.release_transactional_locks();
    return nullptr;
  }
  trans_commit(thd);
  thd->mdl_context.release_transactional_locks();

  mv_rewrite::Catalog catalog(thd, true);
  for (const Mview_def &def : defs) {
    mv_rewrite::View view;
    std::string why;
    bool skip = analyse_view(thd, def, &catalog, registry.get(), &view, &why);
    thd->clear_error();
    if (skip) {
      log_message(INFORMATION_LEVEL, "materialized view " + def.db + "." +
                                         def.name +
                                         " is not used for queries: " + why);
      continue;
    }
    registry->views.push_back(std::move(view));
  }
  if (thd->lex->is_lex_started) {
    thd->end_statement();
    thd->cleanup_after_query();
  }
  registry->tree.build(registry->views);
  return registry;
}

extern "C" void *loader_main(void *) {
  char stack_base = 0;
  my_thread_init();
  std::shared_ptr<Registry> registry;
  THD *thd = make_session(&stack_base);
  if (thd != nullptr) {
    registry = load_registry(thd);
    destroy_session(thd);
  }
  {
    std::lock_guard<std::mutex> guard(registry_mutex);
    if (registry != nullptr) {
      st_loads++;
      st_views.store(registry->views.size());
      current_registry = registry;
    }
    loading = false;
  }
  registry_cond.notify_all();
  my_thread_end();
  return nullptr;
}

/** Start the loader if it does not run (registry_mutex is held). */
bool start_loader(std::unique_lock<std::mutex> &lock) {
  if (loading) return true;
  if (stopping) return false;
  if (loader_joinable) {
    /* it has ended: loading is false */
    lock.unlock();
    void *result;
    my_thread_join(&loader_thread, &result);
    lock.lock();
    loader_joinable = false;
    if (loading) return true;
  }
  my_thread_attr_t attr;
  my_thread_attr_init(&attr);
  my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE);
  if (my_thread_stack_size > 0)
    (void)my_thread_attr_setstacksize(&attr, my_thread_stack_size);
  const int error =
      my_thread_create(&loader_thread, &attr, loader_main, nullptr);
  my_thread_attr_destroy(&attr);
  if (error != 0) return false;
  loading = true;
  loader_joinable = true;
  return true;
}

/** The current registry; loads it if it is out of date. */
std::shared_ptr<const Registry> get_registry(bool can_wait) {
  const uint64_t version = mview_registry_version.load();
  std::unique_lock<std::mutex> lock(registry_mutex);
  if (current_registry != nullptr && current_registry->version == version)
    return current_registry;
  if (!start_loader(lock)) return nullptr;
  if (!can_wait) return nullptr;
  registry_cond.wait(lock, [] { return !loading; });
  if (current_registry != nullptr && current_registry->version == version)
    return current_registry;
  return nullptr;
}

/* ------------------------------------------------------------------ */
/* Read your writes                                                    */
/* ------------------------------------------------------------------ */

/** A position in the binary log: the number of the file, the offset. */
using Binlog_position = std::pair<unsigned long long, unsigned long long>;

/** The number of a binary log file: the digits after the last dot. */
unsigned long long file_number(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot == nullptr ? 0 : strtoull(dot + 1, nullptr, 10);
}

/** The end of the last transaction each session wrote to the binary log
    (sessions that wrote none are not in it). */
std::mutex sessions_mutex;
std::unordered_map<my_thread_id, Binlog_position> last_commit;

/** A transaction was committed: remember where it ends in the binary
    log (the transaction observer of the server calls this). */
int after_commit(Trans_param *param) {
  if (param->log_file == nullptr || param->log_file[0] == '\0' ||
      param->log_pos == 0)
    return 0;
  const Binlog_position position(file_number(param->log_file),
                                 param->log_pos);
  std::lock_guard<std::mutex> guard(sessions_mutex);
  Binlog_position &last = last_commit[param->thread_id];
  if (position > last) last = position;
  return 0;
}

Trans_observer trans_observer = {sizeof(Trans_observer),
                                 nullptr,       // before_dml
                                 nullptr,       // before_commit
                                 nullptr,       // before_rollback
                                 after_commit,  // after_commit
                                 nullptr,       // after_rollback
                                 nullptr};      // begin
MYSQL_PLUGIN plugin_handle = nullptr;

void session_ended(THD *thd) {
  std::lock_guard<std::mutex> guard(sessions_mutex);
  last_commit.erase(thd->thread_id());
}

/** Has the daemon applied what the session committed? */
bool session_is_applied(THD *thd, std::string *why) {
  Binlog_position mine;
  {
    std::lock_guard<std::mutex> guard(sessions_mutex);
    auto it = last_commit.find(thd->thread_id());
    if (it == last_commit.end()) return true;
    mine = it->second;
  }
  std::string file;
  uint64_t pos = 0;
  const bool known = mview_get_applied_position(&file, &pos);
  *why = "session " + std::to_string(mine.first) + ":" +
         std::to_string(mine.second) + " applied " +
         (known ? std::to_string(file_number(file.c_str())) + ":" +
                      std::to_string(pos)
                : "none");
  if (!known) return false;
  return Binlog_position(file_number(file.c_str()), pos) >= mine;
}

/* ------------------------------------------------------------------ */
/* The statement                                                       */
/* ------------------------------------------------------------------ */

/**
  What the text of the statement has that the parse tree does not show
  plainly: a second statement (multi-statements) or optimizer hints
  (which the rewritten statement would not keep).
*/
const char *scan_text(const char *text, size_t length) {
  size_t i = 0;
  while (i < length) {
    const char c = text[i];
    if (c == '\'' || c == '"' || c == '`') {
      i++;
      while (i < length && text[i] != c) {
        if (text[i] == '\\' && c != '`') i++;
        i++;
      }
      i++;
    } else if (c == '#' ||
               (c == '-' && i + 2 < length && text[i + 1] == '-' &&
                (text[i + 2] == ' ' || text[i + 2] == '\t'))) {
      while (i < length && text[i] != '\n') i++;
    } else if (c == '/' && i + 1 < length && text[i + 1] == '*') {
      if (i + 2 < length && text[i + 2] == '+') return "optimizer hints";
      i += 2;
      while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) i++;
      i += 2;
    } else if (c == ';') {
      for (i++; i < length; i++)
        if (!isspace(static_cast<unsigned char>(text[i])) && text[i] != ';')
          return "more than one statement in the query";
      return nullptr;
    } else {
      i++;
    }
  }
  return nullptr;
}

/** Has the transaction of the session changed data (that the views do not
    show yet)? */
bool transaction_has_changes(THD *thd) {
  for (auto &ha_info :
       thd->get_transaction()->ha_trx_info(Transaction_ctx::SESSION)) {
    if (ha_info.is_started() && ha_info.is_trx_read_write()) return true;
  }
  return false;
}

/**
  The session may have its statement rewritten.
  @returns nullptr if it may, else why not
*/
const char *may_rewrite(THD *thd) {
  if (thd->slave_thread || thd->system_thread != NON_SYSTEM_THREAD)
    return "a replication or system thread";
  if (thd->locked_tables_mode != LTM_NONE) return "LOCK TABLES";
  if (thd->in_sub_stmt) return "a statement of a stored program";
  if (transaction_has_changes(thd))
    return "the transaction has changed data that the views do not have";
  if (thd->variables.sql_mode & (MODE_ANSI_QUOTES | MODE_NO_BACKSLASH_ESCAPES))
    return "sql_mode ANSI_QUOTES or NO_BACKSLASH_ESCAPES";
  const CHARSET_INFO *client = thd->variables.character_set_client;
  const CHARSET_INFO *connection = thd->variables.collation_connection;
  if (!my_charset_same(client, connection) ||
      !my_charset_is_ascii_based(client) || client->mbminlen != 1)
    return "character_set_client is not the character set of "
           "collation_connection, or not ASCII based";
  Security_context *sctx = thd->security_context();
  if (sctx->has_global_grant(STRING_WITH_LEN("SKIP_QUERY_REWRITE")).first)
    return "the user has the SKIP_QUERY_REWRITE privilege";
  return scan_text(thd->query().str, thd->query().length);
}

void push_note(THD *thd, const std::string &note) {
  push_warning(thd, Sql_condition::SL_NOTE, ER_UNKNOWN_ERROR,
               ("mview_rewrite: " + note).c_str());
}

int notify(MYSQL_THD thd, mysql_event_class_t event_class,
           const void *event) {
  if (event_class == MYSQL_AUDIT_CONNECTION_CLASS) {
    if (static_cast<const mysql_event_connection *>(event)->event_subclass ==
        MYSQL_AUDIT_CONNECTION_DISCONNECT)
      session_ended(thd);
    return 0;
  }
  if (event_class != MYSQL_AUDIT_PARSE_CLASS) return 0;
  const auto *parse = static_cast<const mysql_event_parse *>(event);
  if (parse->event_subclass != MYSQL_AUDIT_PARSE_POSTPARSE) return 0;
  if (*parse->flags & MYSQL_AUDIT_PARSE_REWRITE_PLUGIN_IS_PREPARED_STATEMENT)
    return 0;
  if (!THDVAR(thd, enabled)) return 0;
  if (thd->lex->sql_command != SQLCOM_SELECT) return 0;
  const bool trace = THDVAR(thd, trace);
  if (const char *why = may_rewrite(thd)) {
    if (trace) push_note(thd, std::string("not rewritten: ") + why);
    return 0;
  }
  st_statements++;

  /* wait for a load only if no lock of ours can hold anyone up */
  const bool can_wait = !thd->mdl_context.has_locks() &&
                        !thd->in_active_multi_stmt_transaction();
  std::shared_ptr<const Registry> registry = get_registry(can_wait);
  if (registry == nullptr) {
    if (trace)
      push_note(thd, "not rewritten: the materialized views are being "
                     "loaded");
    return 0;
  }
  if (registry->views.empty()) return 0;
  /* string literals compare to those of the views in one collation */
  if (thd->variables.collation_connection != registry->collation) {
    if (trace)
      push_note(thd, std::string("not rewritten: collation_connection is "
                                 "not ") +
                         registry->collation->m_coll_name);
    return 0;
  }

  mv_rewrite::Options options;
  std::string position;
  options.sync_current = session_is_applied(thd, &position);
  if (trace && !position.empty()) push_note(thd, "positions: " + position);
  options.use_manual = THDVAR(thd, use_manual_views);
  options.use_foreign_keys = THDVAR(thd, use_foreign_keys);
  options.use_filter_tree = THDVAR(thd, use_filter_tree);
  options.partial = THDVAR(thd, partial);
  options.trace = trace;

  std::string sql;
  std::vector<std::string> notes;
  mv_rewrite::Counters counters;
  bool rewritten = false;
  try {
    rewritten = mv_rewrite::rewrite_statement(thd, *registry, options, &sql,
                                              &notes, &counters);
  } catch (const std::exception &e) {
    log_message(ERROR_LEVEL, std::string("internal error: ") + e.what());
    rewritten = false;
  }
  st_candidates += counters.candidates;
  if (thd->is_error()) thd->clear_error();

  if (rewritten) {
    const std::string original(thd->query().str, thd->query().length);
    MYSQL_LEX_STRING text = {const_cast<char *>(sql.c_str()), sql.size()};
    if (mysql_parser_parse(thd, text, false, swallow_condition, nullptr) !=
        0) {
      log_message(ERROR_LEVEL, "the rewritten statement does not parse: " +
                                   sql + " (from: " + original + ")");
      MYSQL_LEX_STRING orig = {const_cast<char *>(original.c_str()),
                               original.size()};
      /* the statement as it was; its errors are the user's */
      mysql_parser_parse(thd, orig, false, nullptr, nullptr);
      rewritten = false;
    } else {
      *reinterpret_cast<int *>(parse->flags) |=
          static_cast<int>(MYSQL_AUDIT_PARSE_REWRITE_PLUGIN_QUERY_REWRITTEN);
      st_rewritten++;
    }
  }
  for (const std::string &note : notes) push_note(thd, note);
  return 0;
}

st_mysql_audit descriptor = {
    MYSQL_AUDIT_INTERFACE_VERSION,
    nullptr,
    notify,
    {/* general */ 0,
     /* connection */
     static_cast<unsigned long>(MYSQL_AUDIT_CONNECTION_DISCONNECT),
     /* parse */ static_cast<unsigned long>(MYSQL_AUDIT_PARSE_ALL),
     /* authorization */ 0,
     /* table access */ 0,
     /* global variable */ 0,
     /* server startup */ 0,
     /* server shutdown */ 0,
     /* command */ 0,
     /* query */ 0}};

int plugin_init(MYSQL_PLUGIN plugin) {
  if (init_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs)) return 1;
  plugin_handle = plugin;
  if (register_trans_observer(&trans_observer, plugin_handle)) {
    log_message(ERROR_LEVEL, "cannot register the transaction observer");
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }
  std::lock_guard<std::mutex> guard(registry_mutex);
  stopping = false;
  current_registry.reset();
  return 0;
}

int plugin_deinit(void *) {
  unregister_trans_observer(&trans_observer, plugin_handle);
  bool join = false;
  {
    std::unique_lock<std::mutex> lock(registry_mutex);
    stopping = true;
    registry_cond.wait(lock, [] { return !loading; });
    join = loader_joinable;
    loader_joinable = false;
    current_registry.reset();
  }
  {
    std::lock_guard<std::mutex> guard(sessions_mutex);
    last_commit.clear();
  }
  if (join) {
    void *result;
    my_thread_join(&loader_thread, &result);
  }
  deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
  return 0;
}

}  // namespace

mysql_declare_plugin(mview_rewrite){
    MYSQL_AUDIT_PLUGIN,
    &descriptor,
    "MVIEW_REWRITE",
    "Justin Swanhart and contributors",
    "Answers queries from materialized views (view matching of Goldstein "
    "and Larson, SIGMOD 2001).",
    PLUGIN_LICENSE_GPL,
    plugin_init,
    nullptr,
    plugin_deinit,
    0x0100,
    status_variables,
    system_variables,
    nullptr,
    0} mysql_declare_plugin_end;
