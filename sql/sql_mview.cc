/* Copyright (c) 2026, Oracle and/or its affiliates.

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
  @file sql/sql_mview.cc

  CREATE / DROP / REFRESH / ALTER MATERIALIZED VIEW and SHOW CREATE VIEW of
  a materialized view.

  A materialized view is a base table.  The statements analyse the
  definition with the resolver, and then create, fill and drop the table
  with internally generated SQL statements that are executed in the session
  (Ed_connection) with the binary log turned off; the original statement is
  what is written to the binary log, so a replica builds the view from its
  own data.
*/

#include "sql/sql_mview.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lex_string.h"
#include "m_string.h"
#include "my_sys.h"
#include "my_systime.h"
#include "mysql/strings/m_ctype.h"
#include "mysqld_error.h"
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/binlog.h"
#include "sql/tztime.h"  // my_tz_UTC
#include "sql/binlog_index.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dd_schema.h"
#include "sql/dd/properties.h"
#include "sql/dd/types/abstract_table.h"
#include "sql/dd/types/table.h"
#include "sql/derror.h"
#include "sql/enum_query_type.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/protocol_classic.h"
#include "sql/dd/types/column.h"
#include "sql/statement/protocol_local.h"
#include "sql/nested_join.h"
#include "sql/query_options.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_plugin.h"
#include "sql/sql_show.h"
#include "sql/sql_table.h"
#include "sql/sql_tmp_table.h"
#include "sql/statement/ed_connection.h"
#include "sql/strfunc.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/thd_raii.h"
#include "sql/transaction.h"
#include "sql_string.h"

namespace {

const CHARSET_INFO *const sql_text_charset = &my_charset_utf8mb4_bin;

std::string quote(const std::string &name) {
  return mview_quote_identifier(name);
}

std::string qualified(const std::string &db, const std::string &name) {
  return quote(db) + "." + quote(name);
}

bool is_warp(const std::string &engine) {
  return my_strcasecmp(system_charset_info, engine.c_str(), "WARP") == 0;
}

bool has_internal_prefix(const char *name) {
  return native_strncasecmp(name, MVIEW_INTERNAL_PREFIX,
                            strlen(MVIEW_INTERNAL_PREFIX)) == 0;
}

/* ------------------------------------------------------------------ */
/* Metadata locks                                                      */
/* ------------------------------------------------------------------ */

struct Lock_request {
  std::string db;
  std::string name;
  enum_mdl_type type;
};

/**
  Metadata locks held for the whole statement: exclusive locks on the
  tables of the materialized view, shared read-only locks on the base
  tables while the view is filled.  They have explicit duration: the
  internal statements commit and release the transactional locks of the
  session.
*/
class Mview_locks {
 public:
  explicit Mview_locks(THD *thd) : m_thd(thd) {}
  ~Mview_locks() {
    for (MDL_ticket *ticket : m_tickets) m_thd->mdl_context.release_lock(ticket);
  }

  /// Lock all the tables at once (no deadlocks between the tables).
  bool lock(const std::vector<Lock_request> &tables) {
    MDL_request_list requests;
    std::vector<MDL_request *> all;
    std::vector<std::string> schemas;
    for (const Lock_request &table : tables) {
      if (table.type == MDL_EXCLUSIVE &&
          std::find(schemas.begin(), schemas.end(), table.db) ==
              schemas.end()) {
        schemas.push_back(table.db);
        MDL_request *r = new (m_thd->mem_root) MDL_request;
        if (r == nullptr) return true;
        MDL_REQUEST_INIT(r, MDL_key::SCHEMA, table.db.c_str(), "",
                         MDL_INTENTION_EXCLUSIVE, MDL_EXPLICIT);
        requests.push_front(r);
        all.push_back(r);
      }
      MDL_request *r = new (m_thd->mem_root) MDL_request;
      if (r == nullptr) return true;
      MDL_REQUEST_INIT(r, MDL_key::TABLE, table.db.c_str(), table.name.c_str(),
                       table.type, MDL_EXPLICIT);
      requests.push_front(r);
      all.push_back(r);
    }
    if (requests.is_empty()) return false;
    if (m_thd->mdl_context.acquire_locks(&requests,
                                         m_thd->variables.lock_wait_timeout))
      return true;
    for (MDL_request *r : all) m_tickets.push_back(r->ticket);
    return false;
  }

  /// The lock of a table taken by lock(); nullptr if there is none.
  MDL_ticket *find(const std::string &db, const std::string &name) const {
    for (MDL_ticket *ticket : m_tickets) {
      const MDL_key *key = ticket->get_key();
      if (key->mdl_namespace() == MDL_key::TABLE &&
          db == key->db_name() && name == key->name())
        return ticket;
    }
    return nullptr;
  }

 private:
  THD *m_thd;
  std::vector<MDL_ticket *> m_tickets;
};

/** The name of the binlog position table of a view; empty if the name of
    the view is too long to have one. */
std::string state_table_name(const std::string &view) {
  const std::string name = view + MVIEW_STATE_SUFFIX;
  if (system_charset_info->cset->numchars(system_charset_info, name.data(),
                                          name.data() + name.size()) >
      NAME_CHAR_LEN)
    return std::string();
  return name;
}

void add_once(std::vector<Lock_request> *requests, const char *db,
              const char *name, enum_mdl_type type) {
  for (const Lock_request &r : *requests)
    if (r.db == db && r.name == name) return;
  requests->push_back({db, name, type});
}

/* ------------------------------------------------------------------ */
/* Internal statements                                                 */
/* ------------------------------------------------------------------ */

/**
  The environment of the internal statements: the session is in
  maintenance mode, has all the privileges, does not write to the binary
  log, and parses the generated SQL the way it was printed (sql_mode='',
  utf8mb4).  The commits of the internal statements are commits in the
  middle of a statement: they do not consume the GTID of the statement.
*/
class Internal_session {
 public:
  explicit Internal_session(THD *thd)
      : m_thd(thd),
        m_maintenance(thd->mview_maintenance_mode),
        m_sctx(thd->security_context()),
        m_sql_mode(thd->variables.sql_mode),
        m_option_bits(thd->variables.option_bits),
        m_cs_client(thd->variables.character_set_client),
        m_cl_connection(thd->variables.collation_connection),
        m_require_pk(thd->variables.sql_require_primary_key),
        m_generate_pk(thd->variables.sql_generate_invisible_primary_key),
        m_select_limit(thd->variables.select_limit),
        m_time_zone(thd->variables.time_zone),
        m_commit_in_middle(thd->is_commit_in_middle_of_statement),
        m_owned_gtid(thd->owned_gtid),
        m_owned_tsid(thd->owned_tsid),
        m_gtid_next(thd->variables.gtid_next),
        m_substatement_guard(thd) {
    m_internal_sctx.skip_grants();
    thd->set_security_context(&m_internal_sctx);
    thd->mview_maintenance_mode = true;
    thd->variables.sql_mode = 0;
    thd->variables.option_bits &= ~(OPTION_BIN_LOG | OPTION_SAFE_UPDATES);
    thd->variables.option_bits |= OPTION_BIG_SELECTS;
    thd->variables.character_set_client = &my_charset_utf8mb4_0900_ai_ci;
    thd->variables.collation_connection = &my_charset_utf8mb4_0900_ai_ci;
    thd->update_charset();
    thd->variables.sql_require_primary_key = false;
    thd->variables.sql_generate_invisible_primary_key = false;
    thd->variables.select_limit = HA_POS_ERROR;
    /*
      The rows of a view are computed in UTC, by the statement that fills
      it and by the WARP_MVIEW daemon that maintains it: a TIMESTAMP is
      compared, grouped and converted as the date and time of the time zone
      of the session, and only in a time zone without daylight saving time
      is every date and time one point in time (in MET, 2023-10-29 02:30
      is two).  See Mview_def::time_zone_dependent.
    */
    thd->variables.time_zone = my_tz_UTC;
    thd->is_commit_in_middle_of_statement = true;
    /*
      The GTID of the statement (the one a replica applies it with) is not
      seen by the internal statements, whose commits would release it: it
      is for the write of the statement to the binary log, at the end.
    */
    thd->owned_gtid.clear();
    thd->owned_tsid.clear();
    thd->variables.gtid_next.set_automatic();
  }

  ~Internal_session() {
    m_thd->owned_gtid = m_owned_gtid;
    m_thd->owned_tsid = m_owned_tsid;
    m_thd->variables.gtid_next = m_gtid_next;
    m_thd->is_commit_in_middle_of_statement = m_commit_in_middle;
    m_thd->variables.select_limit = m_select_limit;
    m_thd->variables.time_zone = m_time_zone;
    m_thd->variables.sql_generate_invisible_primary_key = m_generate_pk;
    m_thd->variables.sql_require_primary_key = m_require_pk;
    m_thd->variables.character_set_client = m_cs_client;
    m_thd->variables.collation_connection = m_cl_connection;
    m_thd->update_charset();
    m_thd->variables.option_bits = m_option_bits;
    m_thd->variables.sql_mode = m_sql_mode;
    m_thd->mview_maintenance_mode = m_maintenance;
    m_thd->set_security_context(m_sctx);
  }

  /**
    Execute a statement.

    @param sql     The statement.
    @param report  Report the error of the statement as the error of the
                   session.
    @retval true the statement failed
  */
  bool run(const std::string &sql, bool report = true) {
    if (m_thd->killed) {
      if (report) m_thd->send_kill_message();
      return true;
    }
    LEX_STRING text;
    if (lex_string_strmake(m_thd->mem_root, &text, sql.data(), sql.size()))
      return true;
    Ed_connection connection(m_thd);
    /*
      The statement is not a statement of the stored procedure that the
      statement of the user may be in (see Statement_runnable).
    */
    sp_rcontext *const sp_runtime_ctx = m_thd->sp_runtime_ctx;
    m_thd->sp_runtime_ctx = nullptr;
    const bool error = connection.execute_direct(text);
    m_thd->sp_runtime_ctx = sp_runtime_ctx;
    if (error) {
      if (report && !m_thd->is_error()) {
        const unsigned int error = connection.get_last_errno();
        my_message(error != 0 ? error : ER_UNKNOWN_ERROR,
                   connection.get_last_error(), MYF(0));
      }
      return true;
    }
    return false;
  }

  /**
    Execute a statement that returns one result set.

    @param[out] rows  The rows; a NULL is (true, "").
    @retval true the statement failed (reported)
  */
  bool query(const std::string &sql,
             std::vector<std::vector<std::pair<bool, std::string>>> *rows) {
    rows->clear();
    LEX_STRING text;
    if (lex_string_strmake(m_thd->mem_root, &text, sql.data(), sql.size()))
      return true;
    Ed_connection connection(m_thd);
    sp_rcontext *const sp_runtime_ctx = m_thd->sp_runtime_ctx;
    m_thd->sp_runtime_ctx = nullptr;
    const bool error = connection.execute_direct(text);
    m_thd->sp_runtime_ctx = sp_runtime_ctx;
    if (error) {
      if (!m_thd->is_error()) {
        const unsigned int code = connection.get_last_errno();
        my_message(code != 0 ? code : ER_UNKNOWN_ERROR,
                   connection.get_last_error(), MYF(0));
      }
      return true;
    }
    Ed_result_set *result = connection.get_result_sets();
    if (result == nullptr) return false;
    List<Ed_row> &list = *result;
    for (Ed_row &row : list) {
      std::vector<std::pair<bool, std::string>> values;
      for (size_t i = 0; i < row.size(); i++) {
        const Ed_column &column = row[i];
        if (column.str == nullptr)
          values.emplace_back(true, std::string());
        else
          values.emplace_back(false, std::string(column.str, column.length));
      }
      rows->push_back(std::move(values));
    }
    return false;
  }

  THD *thd() const { return m_thd; }

 private:
  THD *m_thd;
  bool m_maintenance;
  Security_context *m_sctx;
  Security_context m_internal_sctx;
  sql_mode_t m_sql_mode;
  ulonglong m_option_bits;
  const CHARSET_INFO *m_cs_client;
  const CHARSET_INFO *m_cl_connection;
  bool m_require_pk;
  bool m_generate_pk;
  ha_rows m_select_limit;
  Time_zone *m_time_zone;
  bool m_commit_in_middle;
  Gtid m_owned_gtid;
  mysql::gtid::Tsid m_owned_tsid;
  Gtid_specification m_gtid_next;
  Implicit_substatement_state_guard m_substatement_guard;
};

/** Commit the transaction of the statement, or roll it back on failure. */
bool commit_or_rollback(THD *thd, bool failed) {
  if (!failed && !thd->is_error() && !trans_commit_stmt(thd) &&
      !trans_commit(thd))
    return false;
  trans_rollback_stmt(thd);
  trans_rollback(thd);
  return true;
}

/** Write the statement of the user to the binary log and commit. */
bool binlog_and_commit(THD *thd) {
  bool failed = false;
  if (mysql_bin_log.is_open() && thd->variables.sql_log_bin &&
      (thd->variables.option_bits & OPTION_BIN_LOG)) {
    failed = write_bin_log(thd, true, thd->query().str, thd->query().length,
                           true);
  }
  return commit_or_rollback(thd, failed);
}

uint64_t next_epoch(uint64_t previous) {
  const uint64_t now = my_micro_time();
  return now > previous ? now : previous + 1;
}

/* ------------------------------------------------------------------ */
/* The data dictionary                                                 */
/* ------------------------------------------------------------------ */

/**
  Store the metadata in the options of the view table.  The caller holds
  an exclusive metadata lock on the table and commits, with a
  Dictionary_client::Auto_releaser that lives until after the commit: the
  commit is what replaces the object in the dictionary cache.
*/
bool store_definition(THD *thd, const Mview_def &def,
                      const std::vector<std::string> &user_columns) {
  dd::Table *table = nullptr;
  if (thd->dd_client()->acquire_for_modification(def.db.c_str(),
                                                 def.name.c_str(), &table))
    return true;
  if (table == nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), def.db.c_str(), def.name.c_str());
    return true;
  }
  const std::string json = mview_def_to_json(def, user_columns);
  table->options().set(MVIEW_DD_KIND_KEY,
                       static_cast<uint32>(MVIEW_VIEW_TABLE));
  table->options().set(MVIEW_DD_DEF_KEY,
                       dd::String_type(json.data(), json.size()));
  return thd->dd_client()->update(table);
}

/** Mark a table as a table of a materialized view; see store_definition().
 */
bool mark_table(THD *thd, const char *db, const char *name,
                Mview_table_kind kind) {
  dd::Table *table = nullptr;
  if (thd->dd_client()->acquire_for_modification(db, name, &table)) return true;
  if (table == nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), db, name);
    return true;
  }
  table->options().set(MVIEW_DD_KIND_KEY, static_cast<uint32>(kind));
  return thd->dd_client()->update(table);
}

/**
  Read the definition of a materialized view.  The caller holds a metadata
  lock on the table.

  @param[out] exists  The table exists (as a table or a view).
  @param[out] is_mview The table is a materialized view; def is filled in.
  @retval true error (reported)
*/
bool load_definition(THD *thd, const char *db, const char *name, bool *exists,
                     bool *is_mview, Mview_def *def,
                     std::vector<std::string> *user_columns) {
  *exists = *is_mview = false;
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Abstract_table *table = nullptr;
  if (thd->dd_client()->acquire(db, name, &table)) return true;
  if (table == nullptr) return false;
  *exists = true;
  if (mview_kind_from_dd(*table) != MVIEW_VIEW_TABLE) return false;
  std::string reason;
  if (mview_def_from_dd(db, *table, def, user_columns, &reason)) {
    my_error(ER_MVIEW_BAD_METADATA, MYF(0), db, name, reason.c_str());
    return true;
  }
  *is_mview = true;
  return false;
}

/* ------------------------------------------------------------------ */
/* Checks of the definition                                            */
/* ------------------------------------------------------------------ */

bool unsupported(const char *what) {
  my_error(ER_MVIEW_UNSUPPORTED_DEFINITION, MYF(0), what);
  return true;
}

bool unsupported_table(const Table_ref *tr, const char *why) {
  my_error(ER_MVIEW_UNSUPPORTED_TABLE, MYF(0), tr->db ? tr->db : "",
           tr->table_name ? tr->table_name : tr->alias, why);
  return true;
}

/** What the checks of the query block learn about its shape. */
struct Mview_analysis {
  bool has_aggregates{false};
  bool is_aggregate{false};
  /// A TIMESTAMP column or expression: see Mview_def::time_zone_dependent.
  bool time_zone_dependent{false};
  /// The expressions the view is grouped by: GROUP BY, or the select list
  /// of SELECT DISTINCT.
  std::vector<Item *> group_items;
};

/** Checks that need only the parse tree; made before the tables are opened,
    the resolver rewrites subqueries and derived tables. */
bool check_parsed_query(LEX *lex) {
  Query_expression *const unit = lex->unit;
  Query_block *const qb = lex->query_block;

  if (!unit->is_simple() || qb->next_query_block() != nullptr)
    return unsupported("set operations (UNION, INTERSECT, EXCEPT)");
  if (unit->m_with_clause != nullptr)
    return unsupported("common table expressions");
  if (qb->get_table_list() == nullptr)
    return unsupported("the SELECT has no tables");
  for (Table_ref *tr = qb->get_table_list(); tr != nullptr;
       tr = tr->next_local) {
    if (tr->is_table_function()) return unsupported("table functions");
    if (tr->is_derived() || tr->derived_query_expression() != nullptr)
      return unsupported("derived tables");
    if (tr->lock_descriptor().type == TL_WRITE ||
        tr->lock_descriptor().type == TL_READ_WITH_SHARED_LOCKS)
      return unsupported("locking clauses (FOR UPDATE, FOR SHARE)");
    /* The maintenance puts other tables in the place of the base tables. */
    if (tr->partition_names != nullptr && tr->partition_names->elements != 0)
      return unsupported("partition selection");
    if (tr->index_hints != nullptr && tr->index_hints->elements != 0)
      return unsupported("index hints");
    if (tr->has_tablesample()) return unsupported("TABLESAMPLE");
  }
  if (qb->first_inner_query_expression() != nullptr)
    return unsupported("subqueries");
  if (qb->has_limit()) return unsupported("LIMIT");
  if (qb->order_list.elements != 0) return unsupported("ORDER BY");
  if (qb->olap != UNSPECIFIED_OLAP_TYPE)
    return unsupported("GROUP BY with ROLLUP, CUBE or GROUPING SETS (a table "
                       "cannot hold super-aggregate rows)");
  if (qb->has_windows()) return unsupported("window functions");
  return false;
}

/** The tables as they were opened: only permanent user base tables. */
bool check_opened_tables(LEX *lex) {
  for (Table_ref *tr = lex->query_tables; tr != nullptr; tr = tr->next_global) {
    if (tr->is_view()) return unsupported_table(tr, "it is a view");
    if (tr->schema_table != nullptr || tr->table == nullptr)
      return unsupported_table(tr, "it is not a base table");
    const TABLE_SHARE *share = tr->table->s;
    if (share->tmp_table != NO_TMP_TABLE)
      return unsupported_table(tr, "it is a temporary table");
    if (share->mview_kind != MVIEW_NOT_A_MVIEW)
      return unsupported_table(tr, "it is a materialized view");
    if (share->table_category != TABLE_CATEGORY_USER ||
        is_infoschema_db(tr->db) || is_perfschema_db(tr->db) ||
        my_strcasecmp(system_charset_info, tr->db, "mysql") == 0 ||
        my_strcasecmp(system_charset_info, tr->db, "sys") == 0)
      return unsupported_table(tr, "it is a system table");
    /* A table without a PRIMARY KEY gets another row key, see
       choose_row_key(). */
  }
  return false;
}

/**
  Can the column be part of a row key made of columns: the maintenance
  compares the values it decodes from row images to it exactly, and puts
  them in IN-lists.
*/
bool row_key_column(const Field *field) {
  if (field->is_hidden_by_system() || field->is_gcol()) return false;
  if (field->is_flag_set(BLOB_FLAG)) return false;  // TEXT, JSON, spatial...
  switch (field->real_type()) {
    case MYSQL_TYPE_FLOAT:  // no exact literal
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_VECTOR:
    case MYSQL_TYPE_TYPED_ARRAY:
    case MYSQL_TYPE_NULL:
      return false;
    default:
      return true;
  }
}

/**
  The row key of a base table instance (see Mview_key_kind): the PRIMARY
  KEY; else the UNIQUE key of NOT NULL columns that the server uses in its
  place; else, for a join view, every column that row_key_column() accepts;
  else (aggregate views do not use it) none.
*/
void choose_row_key(const TABLE *table, bool aggregate, Mview_key_kind *kind,
                    std::vector<const Field *> *fields) {
  fields->clear();
  const TABLE_SHARE *share = table->s;
  if (share->primary_key != MAX_KEY) {
    const KEY &key = table->key_info[share->primary_key];
    bool usable = true;
    for (uint part = 0; part < key.user_defined_key_parts; part++) {
      /* The column: the field of a key part on a prefix is another one. */
      const Field *field = table->field[key.key_part[part].fieldnr - 1];
      if (field->is_hidden_by_system()) usable = false;  // functional
      fields->push_back(field);
    }
    if (usable && !fields->empty()) {
      *kind = strcmp(key.name, "PRIMARY") == 0 ? Mview_key_kind::PRIMARY
                                               : Mview_key_kind::UNIQUE;
      return;
    }
    fields->clear();
  }
  if (aggregate) {
    *kind = Mview_key_kind::NONE;
    return;
  }
  *kind = Mview_key_kind::COLUMNS;
  for (uint f = 0; f < share->fields; f++)
    if (row_key_column(table->field[f])) fields->push_back(table->field[f]);
}

/**
  Check an expression of the definition: it has to give the same value
  whenever and in whatever session it is evaluated.
*/
bool check_expression(Item *root, Mview_analysis *analysis) {
  if (root == nullptr) return false;
  std::string what;
  WalkItem(root, enum_walk::PREFIX, [&](Item *item) -> bool {
    if (item->data_type() == MYSQL_TYPE_TIMESTAMP ||
        item->data_type() == MYSQL_TYPE_TIMESTAMP2)
      analysis->time_zone_dependent = true;
    switch (item->type()) {
      case Item::SUBQUERY_ITEM:
        what = "subqueries";
        return true;
      case Item::PARAM_ITEM:
      case Item::ROUTINE_FIELD_ITEM:
      case Item::TRIGGER_FIELD_ITEM:
        what = "variables and parameters";
        return true;
      case Item::DEFAULT_VALUE_ITEM:
      case Item::INSERT_VALUE_ITEM:
        what = "DEFAULT() and VALUES()";
        return true;
      case Item::SUM_FUNC_ITEM: {
        Item_sum *sum = down_cast<Item_sum *>(item);
        if (sum->m_is_window_function) {
          what = "window functions";
          return true;
        }
        if (sum->sum_func() == Item_sum::UDF_SUM_FUNC) {
          what = "loadable functions";
          return true;
        }
        analysis->has_aggregates = true;
        return false;
      }
      case Item::FUNC_ITEM:
      case Item::COND_ITEM: {
        Item_func *func = down_cast<Item_func *>(item);
        switch (func->functype()) {
          case Item_func::FUNC_SP:
            what = "stored functions";
            return true;
          case Item_func::UDF_FUNC:
            what = "loadable functions";
            return true;
          case Item_func::SUSERVAR_FUNC:
          case Item_func::GUSERVAR_FUNC:
          case Item_func::GSYSVAR_FUNC:
            what = "user and system variables";
            return true;
          case Item_func::FT_FUNC:
            what = "full-text search";
            return true;
          default:
            break;
        }
        Check_function_as_value_generator_parameters params(
            0, VGS_GENERATED_COLUMN);
        if (item->check_function_as_value_generator(
                pointer_cast<uchar *>(&params))) {
          what = "function '";
          what += params.banned_function_name != nullptr
                      ? params.banned_function_name
                      : func->func_name();
          what += "' is not deterministic or depends on the session";
          return true;
        }
        return false;
      }
      default:
        return false;
    }
  });
  if (what.empty()) return false;
  return unsupported(what.c_str());
}

/** Call a function for every ON condition of a join tree. */
template <typename T>
bool for_each_join_condition(const mem_root_deque<Table_ref *> &tables,
                             T &&function) {
  for (Table_ref *tr : tables) {
    if (tr->join_cond() != nullptr && function(tr->join_cond())) return true;
    if (tr->nested_join != nullptr &&
        for_each_join_condition(tr->nested_join->m_tables, function))
      return true;
  }
  return false;
}

/** Checks of the resolved query block; fills in the analysis. */
bool check_resolved_query(Query_block *qb, Mview_analysis *analysis) {
  if (qb->has_windows()) return unsupported("window functions");
  if (qb->has_ft_funcs()) return unsupported("full-text search");
  if (qb->having_cond() != nullptr)
    return unsupported("HAVING (a table cannot hide groups)");
  for (Item *item : qb->visible_fields())
    if (check_expression(item, analysis)) return true;
  if (check_expression(qb->where_cond(), analysis)) return true;
  if (for_each_join_condition(qb->m_table_nest, [&](Item *cond) {
        return check_expression(cond, analysis);
      }))
    return true;
  for (ORDER *group = qb->group_list.first; group != nullptr;
       group = group->next) {
    if (check_expression(*group->item, analysis)) return true;
    analysis->group_items.push_back(*group->item);
  }
  const bool grouped = qb->is_grouped() || analysis->has_aggregates;
  if (qb->is_distinct()) {
    if (grouped)
      return unsupported(
          "SELECT DISTINCT with GROUP BY or aggregate functions");
    /* DISTINCT is GROUP BY all the select expressions. */
    for (Item *item : qb->visible_fields())
      analysis->group_items.push_back(item);
  }
  analysis->is_aggregate = grouped || qb->is_distinct();
  for (Item *item : analysis->group_items) {
    /* GROUP BY <number> would be a position in the generated statements. */
    if (item->const_item())
      return unsupported("constant GROUP BY or DISTINCT expressions");
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* Column definitions                                                  */
/* ------------------------------------------------------------------ */

/**
  The collation of a string column of a WARP table: WARP stores only binary
  strings and strings of binary and case sensitive collations of character
  sets without NUL bytes (see warp_collation_supported in ha_warp.cc).
*/
const CHARSET_INFO *warp_collation(const CHARSET_INFO *cs) {
  if (cs == &my_charset_bin) return cs;
  if (cs->mbminlen == 1) {
    if ((cs->state & (MY_CS_BINSORT | MY_CS_CSSORT)) != 0) return cs;
    const CHARSET_INFO *bin =
        get_charset_by_csname(cs->csname, MY_CS_BINSORT, MYF(0));
    if (bin != nullptr) return bin;
  }
  return &my_charset_utf8mb4_bin;
}

bool is_string_type(const Field *field) {
  switch (field->real_type()) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return true;
    default:
      return false;
  }
}

/** "type [CHARACTER SET cs COLLATE cl]" of a column that holds the values
    of a field. */
std::string column_type(const Field *field, bool warp) {
  if (field->type() == MYSQL_TYPE_NULL) return "binary(0)";
  char buffer[MAX_FIELD_WIDTH];
  String type(buffer, sizeof(buffer), system_charset_info);
  field->sql_type(type);
  std::string def(type.ptr(), type.length());
  if (is_string_type(field) && field->has_charset()) {
    const CHARSET_INFO *cs =
        warp ? warp_collation(field->charset()) : field->charset();
    def += " CHARACTER SET ";
    def += cs->csname;
    def += " COLLATE ";
    def += cs->m_coll_name;
  }
  return def;
}

/**
  "`name` type NULL [INVISIBLE]".  Every column is nullable: a column of an
  outer joined table is NULL in a NULL-extended row, and nothing is gained
  from NOT NULL in a table nobody inserts into.
*/
std::string column_definition(const std::string &name, const std::string &type,
                              bool invisible) {
  return quote(name) + " " + type + (invisible ? " NULL INVISIBLE" : " NULL");
}

/** The field that CREATE TABLE ... SELECT would make for an expression
    (see generate_create_field()). */
Field *result_field(THD *thd, Item *item, TABLE *tmp_table) {
  if (item->type() == Item::FUNC_ITEM) {
    if (item->result_type() != STRING_RESULT || item->returns_array())
      return item->tmp_table_field(tmp_table);
    return item->tmp_table_field_from_field_type(tmp_table, false);
  }
  Field *from_field;
  Field *default_field;
  return create_tmp_field(thd, tmp_table, item, item->type(), nullptr,
                          &from_field, &default_field, false, false, false);
}

/** Can the columns make a secondary index of a table of the engine? */
bool indexable(const std::vector<const Field *> &fields, bool warp) {
  if (fields.empty() || fields.size() > 16) return false;
  size_t length = 0;
  for (const Field *field : fields) {
    switch (field->real_type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:
        break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
        /* WARP indexes integers, temporals, years and whole strings. */
        if (warp) return false;
        break;
      default:
        /* BLOB, JSON, GEOMETRY...: no index without a prefix. */
        return false;
    }
    length += field->pack_length() + 3;
  }
  return length <= 3000;
}

/* ------------------------------------------------------------------ */
/* Building the definition                                             */
/* ------------------------------------------------------------------ */

/** What CREATE needs besides the Mview_def. */
struct Mview_build {
  std::vector<std::string> user_columns;
  std::vector<std::string> column_definitions;
  std::vector<std::string> index_definitions;
};

std::string print_item(THD *thd, const Item *item,
                       enum_query_type type = QT_TO_ARGUMENT_CHARSET) {
  StringBuffer<512> buffer(sql_text_charset);
  item->print(thd, &buffer, type);
  return std::string(buffer.ptr(), buffer.length());
}

/** The names of the user columns: the column list of the statement, or the
    names of the select expressions. */
bool user_column_names(Table_ref *view, Query_block *qb,
                       std::vector<std::string> *names) {
  const Create_col_name_list *list = view->derived_column_names();
  const size_t count = qb->num_visible_fields();
  if (list != nullptr && list->size() != count) {
    my_error(ER_VIEW_WRONG_LIST, MYF(0));
    return true;
  }
  size_t pos = 0;
  for (Item *item : qb->visible_fields()) {
    std::string name;
    if (list != nullptr) {
      name.assign((*list)[pos].str, (*list)[pos].length);
    } else {
      if (item->item_name.is_set())
        name.assign(item->item_name.ptr(), item->item_name.length());
      /* An expression without a usable name gets one, as in a view. */
      Name_string checked;
      checked.copy(name.c_str(), name.length(), system_charset_info);
      if (name.empty() || name.length() > NAME_LEN ||
          check_column_name(checked))
        name = "Name_exp_" + std::to_string(pos + 1);
    }
    if (has_internal_prefix(name.c_str())) {
      my_error(ER_WRONG_COLUMN_NAME, MYF(0), name.c_str());
      return true;
    }
    for (const std::string &other : *names) {
      if (my_strcasecmp(system_charset_info, other.c_str(), name.c_str()) ==
          0) {
        my_error(ER_DUP_FIELDNAME, MYF(0), name.c_str());
        return true;
      }
    }
    names->push_back(std::move(name));
    pos++;
  }
  return false;
}

/**
  The parts that join views and aggregate views share: the table
  instances, the columns they are referenced by, FROM and WHERE.

  Every column reference of the definition is made to print as
  `alias`.`column`, so that the maintenance can put another table with the
  same columns under the alias (see Mview_def::from_template).
*/
bool build_tables(THD *thd, Query_block *qb,
                  const std::vector<Item *> &group_items, bool aggregate,
                  Mview_def *def) {
  /* The definition, for SHOW CREATE VIEW, as a view prints it. */
  {
    StringBuffer<1024> buffer(sql_text_charset);
    thd->lex->unit->print(thd, &buffer, QT_TO_ARGUMENT_CHARSET);
    def->definition.assign(buffer.ptr(), buffer.length());
  }

  /* The table instances, in the order of the leaves of the join. */
  std::vector<Table_ref *> leaves;
  std::vector<std::vector<const Field *>> row_keys;
  bool root_found = false;
  for (Table_ref *tr = qb->leaf_tables; tr != nullptr; tr = tr->next_leaf) {
    Mview_table mt;
    mt.db = tr->db;
    mt.table = tr->table_name;
    mt.alias = tr->alias;
    mt.null_supplying = tr->is_inner_table_of_outer_join();
    for (const Mview_table &other : def->tables) {
      if (my_strcasecmp(table_alias_charset, other.alias.c_str(),
                        mt.alias.c_str()) == 0) {
        my_error(ER_NONUNIQ_TABLE, MYF(0), mt.alias.c_str());
        return true;
      }
    }
    if (!root_found && !mt.null_supplying) {
      def->root = def->tables.size();
      root_found = true;
    }
    std::vector<const Field *> key_fields;
    choose_row_key(tr->table, aggregate, &mt.key_kind, &key_fields);
    for (const Field *field : key_fields) {
      mt.pk_columns.emplace_back(field->field_name);
      mt.pk_ordinals.push_back(0);  // see mview_fill_pk_ordinals()
    }
    def->tables.push_back(std::move(mt));
    leaves.push_back(tr);
    row_keys.push_back(std::move(key_fields));
  }
  if (!root_found)
    return unsupported("every table is on the inner side of an outer join");

  /* The referenced columns; they print as `alias`.`column` from now on. */
  std::vector<std::vector<bool>> referenced(leaves.size());
  for (size_t i = 0; i < leaves.size(); i++) {
    const TABLE *table = leaves[i]->table;
    referenced[i].assign(table->s->fields, false);
    for (const Field *field : row_keys[i])
      referenced[i][field->field_index()] = true;
  }
  bool foreign_column = false;
  auto collect = [&](Item *root) -> bool {
    if (root == nullptr) return false;
    WalkItem(root, enum_walk::PREFIX, [&](Item *item) -> bool {
      if (item->type() != Item::FIELD_ITEM) return false;
      Item_field *field = down_cast<Item_field *>(item);
      size_t i = 0;
      while (i < leaves.size() && leaves[i]->table != field->field->table) i++;
      if (i == leaves.size()) {
        foreign_column = true;
        return true;
      }
      referenced[i][field->field->field_index()] = true;
      field->db_name = nullptr;
      return false;
    });
    return foreign_column;
  };
  for (Item *item : qb->visible_fields()) collect(item);
  for (Item *item : group_items) collect(item);
  collect(qb->where_cond());
  for_each_join_condition(qb->m_table_nest, collect);
  if (foreign_column) return unsupported("a column of an unknown table");
  for (size_t i = 0; i < leaves.size(); i++) {
    const TABLE *table = leaves[i]->table;
    for (uint f = 0; f < table->s->fields; f++) {
      if (referenced[i][f])
        def->tables[i].ref_columns.emplace_back(table->field[f]->field_name);
    }
    def->tables[i].ref_ordinals.assign(def->tables[i].ref_columns.size(), 0);
  }

  /* FROM, and FROM with placeholders for the tables. */
  for (int pass = 0; pass < 2; pass++) {
    struct Saved {
      const char *db, *table_name;
      size_t db_length, table_name_length;
    };
    std::vector<Saved> saved;
    std::vector<std::string> sources;
    if (pass == 1) {
      sources.reserve(leaves.size());
      for (size_t i = 0; i < leaves.size(); i++) {
        Table_ref *tr = leaves[i];
        saved.push_back(
            {tr->db, tr->table_name, tr->db_length, tr->table_name_length});
        sources.push_back(std::string(MVIEW_INTERNAL_PREFIX) + "source$" +
                          std::to_string(i));
        tr->db = "";
        tr->db_length = 0;
        tr->table_name = sources.back().c_str();
        tr->table_name_length = sources.back().length();
      }
    }
    StringBuffer<1024> buffer(sql_text_charset);
    qb->print_from_clause(thd, &buffer, QT_TO_ARGUMENT_CHARSET);
    for (size_t i = 0; i < saved.size(); i++) {
      Table_ref *tr = leaves[i];
      tr->db = saved[i].db;
      tr->db_length = saved[i].db_length;
      tr->table_name = saved[i].table_name;
      tr->table_name_length = saved[i].table_name_length;
    }
    const std::string from(buffer.ptr(), buffer.length());
    const size_t start = from.find("from ");
    if (start == std::string::npos) return unsupported("no FROM clause");
    (pass == 0 ? def->from_sql : def->from_template) =
        "FROM " + from.substr(start + 5);
  }
  if (qb->where_cond() != nullptr)
    def->where_sql = print_item(thd, qb->where_cond());
  return false;
}

/** The layout and the metadata of a join view. */
bool build_join_view(THD *thd, Table_ref *view, Query_block *qb, bool warp,
                     Mview_def *def, Mview_build *build) {
  if (user_column_names(view, qb, &build->user_columns)) return true;

  TABLE_SHARE share;
  init_tmp_table_share(thd, &share, "", 0, "", "", nullptr);
  TABLE tmp_table;
  tmp_table.s = &share;
  tmp_table.set_not_started();

  /* The user columns. */
  def->detail_select = "SELECT ";
  /* The user columns that are a column of a table instance, as they are */
  struct Plain_column {
    const Field *base;
    const std::string *name;
    std::string type;
  };
  std::vector<Plain_column> plain_columns;
  size_t pos = 0;
  for (Item *item : qb->visible_fields()) {
    const std::string &name = build->user_columns[pos];
    Field *field = result_field(thd, item, &tmp_table);
    if (field == nullptr) return true;
    const std::string type = column_type(field, warp);
    build->column_definitions.push_back(column_definition(name, type, false));
    if (item->real_item()->type() == Item::FIELD_ITEM) {
      const Field *base = down_cast<Item_field *>(item->real_item())->field;
      if (base != nullptr) plain_columns.push_back({base, &name, type});
    }
    if (pos != 0) {
      def->detail_select += ", ";
      def->detail_columns += ", ";
    }
    def->detail_select += print_item(thd, item) + " AS " + quote(name);
    def->detail_columns += quote(name);
    pos++;
  }

  /* The key columns: the row key of every table instance (see
     choose_row_key()), in a column of the user that is exactly the column
     of the instance, else in an INVISIBLE column. */
  size_t instance = 0;
  for (Table_ref *tr = qb->leaf_tables; tr != nullptr;
       tr = tr->next_leaf, instance++) {
    Mview_table &mt = def->tables[instance];
    const TABLE *table = tr->table;
    Mview_key_kind kind;
    std::vector<const Field *> key_fields;
    choose_row_key(table, false, &kind, &key_fields);
    assert(kind == mt.key_kind && key_fields.size() == mt.pk_columns.size());
    std::vector<std::string> index_columns;
    for (size_t part = 0; part < key_fields.size(); part++) {
      const Field *field = key_fields[part];
      const std::string type = column_type(field, warp);
      std::string key_column;
      for (const Plain_column &plain : plain_columns) {
        if (plain.base->table == table &&
            plain.base->field_index() == field->field_index() &&
            plain.type == type) {
          key_column = *plain.name;
          break;
        }
      }
      if (key_column.empty()) {
        key_column = std::string(MVIEW_INTERNAL_PREFIX) + "pk" +
                     std::to_string(instance) + "_" + std::to_string(part);
        build->column_definitions.push_back(
            column_definition(key_column, type, true));
        def->detail_select += ", " + quote(mt.alias) + "." +
                              quote(field->field_name) + " AS " +
                              quote(key_column);
        def->detail_columns += ", " + quote(key_column);
      }
      mt.key_columns.push_back(key_column);
      index_columns.push_back(quote(key_column));
    }
    /* A wide key of all the columns of a table: its first columns that
       can be indexed narrow the rows down just as well. */
    size_t indexed = std::min<size_t>(key_fields.size(), 16);
    while (indexed > 0 &&
           !indexable(std::vector<const Field *>(key_fields.begin(),
                                                 key_fields.begin() + indexed),
                      warp))
      indexed--;
    if (indexed > 0) {
      std::string columns;
      for (size_t k = 0; k < indexed; k++)
        columns += (k ? ", " : "") + index_columns[k];
      build->index_definitions.push_back(
          "KEY " +
          quote(std::string(MVIEW_INTERNAL_PREFIX) + "ix_pk" +
                std::to_string(instance)) +
          " (" + columns + ")");
    }
  }

  /* An INVISIBLE AUTO_INCREMENT PRIMARY KEY: the statements that fill the
     view never name it (detail_columns), the engine numbers the rows. */
  build->column_definitions.push_back(
      quote(std::string(MVIEW_INTERNAL_PREFIX) + "row_id") +
      " bigint unsigned NOT NULL AUTO_INCREMENT INVISIBLE PRIMARY KEY");
  return false;
}

/* ------------------------------------------------------------------ */
/* Aggregate views                                                     */
/* ------------------------------------------------------------------ */

/** An item that prints as a piece of SQL: a column of the view table, or
    the value of an aggregate function computed from such columns. */
class Item_mview_sql final : public Item_null {
 public:
  explicit Item_mview_sql(LEX_CSTRING sql) : m_sql(sql) {}
  void print(const THD *, String *str, enum_query_type) const override {
    str->append(m_sql.str, m_sql.length);
  }

 private:
  LEX_CSTRING m_sql;  ///< In the memory of the statement.
};

/**
  Builds the layout of an aggregate view: the group columns, the row count,
  the aggregate states, and the user columns as generated columns over
  those.
*/
class Aggregate_builder {
 public:
  Aggregate_builder(THD *thd, bool warp, const std::vector<Item *> &group_items,
                    Mview_def *def, TABLE *tmp_table)
      : m_thd(thd),
        m_warp(warp),
        m_group_items(group_items),
        m_def(def),
        m_tmp_table(tmp_table) {}

  /**
    The expression of the generated column of a select expression: the
    expression, with the GROUP BY expressions and the aggregate functions
    in it replaced by columns of the view table.  The arguments of the
    functions of the expression are replaced in place.

    @returns the new expression, nullptr on error (reported)
  */
  Item *rewrite(Item *item) {
    for (size_t i = 0; i < m_group_items.size(); i++) {
      if (item == m_group_items[i] || item->eq(m_group_items[i]) ||
          item->real_item() == m_group_items[i]->real_item())
        return sql_item(quote(m_def->view_group_columns[i]));
    }
    switch (item->type()) {
      case Item::SUM_FUNC_ITEM:
        return finalise(down_cast<Item_sum *>(item));
      case Item::REF_ITEM:
        return rewrite(down_cast<Item_ref *>(item)->ref_item());
      case Item::FIELD_ITEM: {
        const std::string what =
            "column " + print_item(m_thd, item) +
            " must be a GROUP BY expression or inside an aggregate function";
        unsupported(what.c_str());
        return nullptr;
      }
      case Item::COND_ITEM: {
        List_iterator<Item> it(*down_cast<Item_cond *>(item)->argument_list());
        while (Item *arg = it++) {
          Item *replacement = rewrite(arg);
          if (replacement == nullptr) return nullptr;
          if (replacement != arg) it.replace(replacement);
        }
        return item;
      }
      case Item::FUNC_ITEM: {
        Item_func *func = down_cast<Item_func *>(item);
        Item **args = func->arguments();
        for (uint i = 0; i < func->argument_count(); i++) {
          Item *replacement = rewrite(args[i]);
          if (replacement == nullptr) return nullptr;
          args[i] = replacement;
        }
        return item;
      }
      default:
        break;
    }
    /* A constant stays; anything else that has columns is not known. */
    const bool has_columns =
        WalkItem(item, enum_walk::PREFIX, [](Item *sub) -> bool {
          return sub->type() == Item::FIELD_ITEM ||
                 sub->type() == Item::SUM_FUNC_ITEM;
        });
    if (has_columns) {
      unsupported("this kind of expression over GROUP BY expressions and "
                  "aggregate functions");
      return nullptr;
    }
    return item;
  }

  const std::vector<std::string> &state_definitions() const {
    return m_state_definitions;
  }

 private:
  Item *sql_item(const std::string &sql) {
    const char *text = m_thd->strmake(sql.data(), sql.size());
    if (text == nullptr) return nullptr;
    return new (m_thd->mem_root) Item_mview_sql({text, sql.size()});
  }

  /** The state of a kind and an argument; made if there is none yet. */
  int state(Mview_agg_state::Kind kind, const std::string &arg_sql,
            const std::string &full_sql, const std::string &type,
            int count_state) {
    for (size_t i = 0; i < m_def->states.size(); i++) {
      if (m_def->states[i].kind == kind && m_def->states[i].full_sql == full_sql)
        return static_cast<int>(i);
    }
    Mview_agg_state s;
    s.kind = kind;
    s.column = std::string(MVIEW_INTERNAL_PREFIX) + "s" +
               std::to_string(m_def->states.size());
    s.arg_sql = arg_sql;
    s.full_sql = full_sql;
    s.count_state = count_state;
    m_state_definitions.push_back(column_definition(s.column, type, true));
    m_def->states.push_back(std::move(s));
    return static_cast<int>(m_def->states.size() - 1);
  }

  const std::string &column(int state_index) const {
    return m_def->states[state_index].column;
  }

  int count_state(const std::string &arg_sql) {
    return state(Mview_agg_state::COUNT, arg_sql, "count(" + arg_sql + ")",
                 "bigint", -1);
  }

  /** SUM(arg), of the type that SUM() gives, paired with COUNT(arg). */
  int sum_state(Item *arg, const std::string &arg_sql) {
    const int count = count_state(arg_sql);
    std::string type = "double";
    if (arg->result_type() == INT_RESULT ||
        arg->result_type() == DECIMAL_RESULT) {
      const uint precision = std::min<uint>(
          arg->decimal_precision() + DECIMAL_LONGLONG_DIGITS,
          DECIMAL_MAX_PRECISION);
      const uint scale = std::min<uint>(arg->decimals, DECIMAL_MAX_SCALE);
      type = "decimal(" + std::to_string(precision) + "," +
             std::to_string(scale) + ")";
    }
    return state(Mview_agg_state::SUM, arg_sql, "sum(" + arg_sql + ")", type,
                 count);
  }

  /** The value of an aggregate function from the columns of the view. */
  Item *finalise(Item_sum *sum) {
    const std::string full_sql = print_item(m_thd, sum);
    const bool one_arg = sum->argument_count() == 1;
    Item *arg = one_arg ? sum->get_arg(0) : nullptr;
    const std::string arg_sql = one_arg ? print_item(m_thd, arg) : "";
    switch (sum->sum_func()) {
      case Item_sum::COUNT_FUNC:
        if (!one_arg || sum->has_with_distinct()) break;
        /* COUNT(*), COUNT(1): the number of rows. */
        if (arg->const_item() && !arg->is_nullable())
          return sql_item(quote(m_def->count_column));
        return sql_item(quote(column(count_state(arg_sql))));
      case Item_sum::SUM_FUNC:
        if (!one_arg || sum->has_with_distinct()) break;
        return sql_item(quote(column(sum_state(arg, arg_sql))));
      case Item_sum::AVG_FUNC: {
        if (!one_arg || sum->has_with_distinct()) break;
        const int s = sum_state(arg, arg_sql);
        const int c = m_def->states[s].count_state;
        return sql_item("(" + quote(column(s)) + " / nullif(" +
                        quote(column(c)) + ",0))");
      }
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC: {
        if (!one_arg) break;
        Field *field = result_field(m_thd, sum, m_tmp_table);
        if (field == nullptr) return nullptr;
        const bool is_min = sum->sum_func() == Item_sum::MIN_FUNC;
        return sql_item(quote(column(state(
            is_min ? Mview_agg_state::MIN : Mview_agg_state::MAX, arg_sql,
            std::string(is_min ? "min(" : "max(") + arg_sql + ")",
            column_type(field, m_warp), -1))));
      }
      default:
        break;
    }
    /* Any other aggregate function: its value is stored and recomputed. */
    Field *field = result_field(m_thd, sum, m_tmp_table);
    if (field == nullptr) return nullptr;
    return sql_item(quote(column(state(Mview_agg_state::RECOMPUTE, arg_sql,
                                       full_sql, column_type(field, m_warp),
                                       -1))));
  }

  THD *m_thd;
  bool m_warp;
  const std::vector<Item *> &m_group_items;
  Mview_def *m_def;
  TABLE *m_tmp_table;
  std::vector<std::string> m_state_definitions;
};

/** The layout and the metadata of an aggregate view. */
bool build_aggregate_view(THD *thd, Table_ref *view, Query_block *qb, bool warp,
                          const std::vector<Item *> &group_items,
                          Mview_def *def, Mview_build *build) {
  if (user_column_names(view, qb, &build->user_columns)) return true;

  def->is_aggregate = true;
  def->state_db = def->db;
  def->state_name = state_table_name(def->name);
  if (def->state_name.empty()) {
    my_error(ER_TOO_LONG_IDENT, MYF(0), def->name.c_str());
    return true;
  }
  def->count_column = std::string(MVIEW_INTERNAL_PREFIX) + "count";

  TABLE_SHARE share;
  init_tmp_table_share(thd, &share, "", 0, "", "", nullptr);
  TABLE tmp_table;
  tmp_table.s = &share;
  tmp_table.set_not_started();

  /* The group columns and their index. */
  std::vector<const Field *> group_fields;
  std::string index_columns;
  for (size_t i = 0; i < group_items.size(); i++) {
    const std::string name =
        std::string(MVIEW_INTERNAL_PREFIX) + "g" + std::to_string(i);
    Field *field = result_field(thd, group_items[i], &tmp_table);
    if (field == nullptr) return true;
    def->group_exprs.push_back(print_item(thd, group_items[i]));
    def->view_group_columns.push_back(name);
    build->column_definitions.push_back(
        column_definition(name, column_type(field, warp), true));
    group_fields.push_back(field);
    if (i != 0) index_columns += ", ";
    index_columns += quote(name);
  }
  if (indexable(group_fields, warp))
    build->index_definitions.push_back(
        "KEY " + quote(std::string(MVIEW_INTERNAL_PREFIX) + "ix_group") +
        " (" + index_columns + ")");
  build->column_definitions.push_back(
      column_definition(def->count_column, "bigint", true));

  /*
    The user columns: their types first, from the expressions as they are;
    then the expressions over the columns of the view.
  */
  std::vector<std::string> types;
  for (Item *item : qb->visible_fields()) {
    Field *field = result_field(thd, item, &tmp_table);
    if (field == nullptr) return true;
    types.push_back(column_type(field, warp));
  }
  Aggregate_builder builder(thd, warp, group_items, def, &tmp_table);
  std::vector<std::string> user_definitions;
  size_t pos = 0;
  for (Item *item : qb->visible_fields()) {
    Item *expr = builder.rewrite(item);
    if (expr == nullptr) return true;
    user_definitions.push_back(
        quote(build->user_columns[pos]) + " " + types[pos] +
        " GENERATED ALWAYS AS (" +
        print_item(thd, expr,
                   enum_query_type(QT_TO_ARGUMENT_CHARSET | QT_NO_DB |
                                   QT_NO_TABLE)) +
        ") STORED NULL");
    pos++;
  }
  for (const std::string &state : builder.state_definitions())
    build->column_definitions.push_back(state);
  for (const std::string &user : user_definitions)
    build->column_definitions.push_back(user);

  /* Deltas can be added if no row is NULL-extended and every state is one
     that can be added to. */
  def->delta_capable = true;
  for (const Mview_table &mt : def->tables)
    if (mt.null_supplying) def->delta_capable = false;
  for (const Mview_agg_state &state : def->states)
    if (state.kind == Mview_agg_state::RECOMPUTE) def->delta_capable = false;
  return false;
}

/* ------------------------------------------------------------------ */
/* Filling the view                                                    */
/* ------------------------------------------------------------------ */

/** The statement that computes all the rows of the view. */
std::string insert_statement(const Mview_def &def) {
  std::string sql = "INSERT INTO " + qualified(def.db, def.name) + " (";
  if (!def.is_aggregate) {
    sql += def.detail_columns + ") " + def.detail_select;
  } else {
    std::string select;
    for (size_t i = 0; i < def.group_exprs.size(); i++) {
      sql += quote(def.view_group_columns[i]) + ", ";
      select += def.group_exprs[i] + ", ";
    }
    sql += quote(def.count_column);
    select += "count(0)";
    for (const Mview_agg_state &state : def.states) {
      sql += ", " + quote(state.column);
      select += ", " + state.full_sql;
    }
    sql += ") SELECT " + select;
  }
  sql += " " + def.from_sql;
  if (!def.where_sql.empty()) sql += " WHERE " + def.where_sql;
  for (size_t i = 0; i < def.group_exprs.size(); i++)
    sql += (i == 0 ? " GROUP BY " : ", ") + def.group_exprs[i];
  return sql;
}

std::string string_literal(const std::string &text) {
  std::string res = "'";
  for (const char c : text) {
    if (c == '\'' || c == '\\') res.push_back('\\');
    res.push_back(c);
  }
  res.push_back('\'');
  return res;
}

/* ------------------------------------------------------------------ */
/* Filling a join view in parallel                                     */
/* ------------------------------------------------------------------ */

/*
  The rows of a join view are divided by ranges of an integer column of its
  root table: every row of the view has exactly one row of the root (it is
  never NULL-extended), so the ranges divide the rows of the view.  Sessions
  of their own fill the ranges, each in one transaction; when all of them
  are done they all commit, or all roll back.  While they run the statement
  holds the view with MDL_SHARED_UPGRADABLE instead of MDL_EXCLUSIVE (no
  other DDL; the sessions can write to it) and the base tables with
  MDL_SHARED_READ_ONLY as before (the sessions can read them, nobody can
  change them).  Readers of the view see no rows of the sessions before
  they commit.
*/

using Wide = __int128;

}  // namespace

ulonglong mview_populate_parallel = 0;
ulonglong mview_populate_sessions = 0;

namespace {

bool parse_wide(const std::string &text, Wide *value) {
  size_t i = 0;
  const bool negative = !text.empty() && text[0] == '-';
  if (negative) i = 1;
  if (i >= text.size()) return true;
  Wide v = 0;
  for (; i < text.size(); i++) {
    if (text[i] < '0' || text[i] > '9') return true;
    v = v * 10 + (text[i] - '0');
  }
  *value = negative ? -v : v;
  return false;
}

std::string wide_string(Wide value) {
  if (value == 0) return "0";
  const bool negative = value < 0;
  unsigned __int128 u = negative ? -static_cast<unsigned __int128>(value)
                                 : static_cast<unsigned __int128>(value);
  std::string text;
  while (u != 0) {
    text.push_back(static_cast<char>('0' + static_cast<int>(u % 10)));
    u /= 10;
  }
  if (negative) text.push_back('-');
  std::reverse(text.begin(), text.end());
  return text;
}

/**
  Conditions that divide the rows of a join view into at most `parts`
  ranges of an integer column of its root table (the first of its row key,
  else of the columns the view references), plus the rows where the column
  is NULL.  Empty if there is no such column or it has too few values.
*/
bool split_rows(Internal_session *session, const Mview_def &def, size_t parts,
                std::vector<std::string> *conditions) {
  conditions->clear();
  const Mview_table &root = def.tables[def.root];
  THD *thd = session->thd();
  const dd::Table *table = nullptr;
  if (thd->dd_client()->acquire(root.db.c_str(), root.table.c_str(), &table))
    return true;
  if (table == nullptr) return false;
  auto is_integer = [&](const std::string &name) {
    for (const dd::Column *column : table->columns()) {
      if (my_strcasecmp(system_charset_info, column->name().c_str(),
                        name.c_str()) != 0)
        continue;
      if (column->is_virtual()) return false;
      switch (column->type()) {
        case dd::enum_column_types::TINY:
        case dd::enum_column_types::SHORT:
        case dd::enum_column_types::INT24:
        case dd::enum_column_types::LONG:
        case dd::enum_column_types::LONGLONG:
          return true;
        default:
          return false;
      }
    }
    return false;
  };
  std::string column;
  for (const std::vector<std::string> *names :
       {&root.pk_columns, &root.ref_columns}) {
    for (const std::string &name : *names) {
      if (is_integer(name)) {
        column = name;
        break;
      }
    }
    if (!column.empty()) break;
  }
  if (column.empty()) return false;

  std::vector<std::vector<std::pair<bool, std::string>>> rows;
  /* as text: the internal protocol stores numbers in binary */
  if (session->query("SELECT CAST(MIN(" + quote(column) +
                         ") AS CHAR), CAST(MAX(" + quote(column) +
                         ") AS CHAR) FROM " + qualified(root.db, root.table),
                     &rows))
    return true;
  Wide low, high;
  if (rows.size() != 1 || rows[0].size() != 2 || rows[0][0].first ||
      rows[0][1].first || parse_wide(rows[0][0].second, &low) ||
      parse_wide(rows[0][1].second, &high) || high < low)
    return false;
  const Wide span = high - low + 1;
  const Wide n = std::min<Wide>(span, static_cast<Wide>(parts));
  if (n < 2) return false;
  const std::string ref = quote(root.alias) + "." + quote(column);
  for (Wide i = 0; i < n; i++) {
    const Wide from = low + span * i / n;
    if (i + 1 < n)
      conditions->push_back(ref + " >= " + wide_string(from) + " AND " + ref +
                            " < " + wide_string(low + span * (i + 1) / n));
    else
      conditions->push_back(ref + " >= " + wide_string(from) + " AND " + ref +
                            " <= " + wide_string(high));
  }
  conditions->push_back(ref + " IS NULL");
  return false;
}

/** What the sessions that fill a view share. */
struct Populate_work {
  /// INSERT ... SELECT ... WHERE [(<where>) AND ]: a condition follows.
  std::string statement;
  std::vector<std::string> conditions;
  ulong lock_wait_timeout{0};
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<THD *> sessions;  ///< to kill them
  size_t running{0};  ///< sessions that have not filled their ranges yet
  int decision{0};    ///< 1: commit, -1: roll back
  uint error{0};
  std::string message;

  void fail(uint code, const char *text) {
    std::lock_guard<std::mutex> guard(mutex);
    if (error == 0) {
      error = code != 0 ? code : ER_UNKNOWN_ERROR;
      message = text;
    }
    stop = true;
  }
  void fail(THD *thd) {
    if (thd->is_error())
      fail(thd->get_stmt_da()->mysql_errno(),
           thd->get_stmt_da()->message_text());
    else
      fail(ER_QUERY_INTERRUPTED, ER_THD(thd, ER_QUERY_INTERRUPTED));
  }
};

/** A session for a thread that fills a view, as the sessions of the
    maintenance daemon (storage/warp/warp_mview_daemon.cc). */
THD *make_populate_session(const void *stack_base, ulong lock_wait_timeout) {
  THD *thd = new (std::nothrow) THD;
  if (thd == nullptr) return nullptr;
  thd->set_new_thread_id();
  thd->thread_stack = static_cast<const char *>(stack_base);
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
  thd->variables.lock_wait_timeout = lock_wait_timeout;
  /* as the session of the statement, see Internal_session */
  thd->variables.time_zone = my_tz_UTC;
  Global_THD_manager::get_instance()->add_thd(thd);
  return thd;
}

void destroy_populate_session(THD *thd) {
  thd->get_protocol_classic()->end_net();
  thd->release_resources();
  Global_THD_manager::get_instance()->remove_thd(thd);
  delete thd;
}

/** A thread that fills ranges until there are none left, then waits for
    the decision to commit or roll back. */
void populate_worker(Populate_work *work) {
  my_thread_init();
  int stack_base;
  THD *thd = make_populate_session(&stack_base, work->lock_wait_timeout);
  if (thd == nullptr) {
    work->fail(ER_OUT_OF_RESOURCES, ER_DEFAULT(ER_OUT_OF_RESOURCES));
  } else {
    std::lock_guard<std::mutex> guard(work->mutex);
    work->sessions.push_back(thd);
  }
  bool failed = thd == nullptr;
  if (thd != nullptr) {
    Internal_session session(thd);
    /* one transaction for all the ranges */
    thd->variables.option_bits &= ~OPTION_AUTOCOMMIT;
    thd->variables.option_bits |= OPTION_NOT_AUTOCOMMIT;
    while (!work->stop) {
      const size_t i = work->next++;
      if (i >= work->conditions.size()) break;
      if (session.run(work->statement + "(" + work->conditions[i] + ")")) {
        failed = true;
        work->fail(thd);
        break;
      }
    }
    {
      std::unique_lock<std::mutex> guard(work->mutex);
      work->running--;
      work->changed.notify_all();
      work->changed.wait(guard, [work] { return work->decision != 0; });
    }
    const bool commit = work->decision > 0 && !failed;
    if (commit_or_rollback(thd, !commit) && commit) work->fail(thd);
  } else {
    std::lock_guard<std::mutex> guard(work->mutex);
    work->running--;
    work->changed.notify_all();
  }
  if (thd != nullptr) {
    {
      std::lock_guard<std::mutex> guard(work->mutex);
      work->sessions.erase(
          std::find(work->sessions.begin(), work->sessions.end(), thd));
    }
    destroy_populate_session(thd);
  }
  my_thread_end();
}

/**
  Fill an empty join view with mview_populate_threads sessions.

  @param[out] done  the view was filled (or the sessions failed); false
                    if it is for the caller to fill it alone
  @retval true error (reported); the rows are not committed, or only some
               of them if a commit failed
*/
bool populate_parallel(Internal_session *session, const Mview_def &def,
                       Mview_locks *locks, bool *done) {
  *done = false;
  THD *thd = session->thd();
  size_t threads = thd->variables.mview_populate_threads;
  if (threads == 0)
    threads = std::min<size_t>(
        32, std::max<size_t>(1, std::thread::hardware_concurrency()));
  if (threads < 2 || def.is_aggregate || locks == nullptr) return false;
  MDL_ticket *ticket = locks->find(def.db, def.name);
  if (ticket == nullptr || ticket->get_type() != MDL_EXCLUSIVE) return false;

  Populate_work work;
  if (split_rows(session, def, threads * 8, &work.conditions)) return true;
  if (work.conditions.empty()) return false;
  *done = true;
  work.statement = "INSERT INTO " + qualified(def.db, def.name) + " (" +
                   def.detail_columns + ") " + def.detail_select + " " +
                   def.from_sql + " WHERE ";
  if (!def.where_sql.empty()) work.statement += "(" + def.where_sql + ") AND ";
  work.lock_wait_timeout = thd->variables.lock_wait_timeout;
  const size_t workers = std::min(threads, work.conditions.size());

  ticket->downgrade_lock(MDL_SHARED_UPGRADABLE);
  __atomic_fetch_add(&mview_populate_parallel, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&mview_populate_sessions, workers, __ATOMIC_RELAXED);
  std::vector<std::thread> pool;
  work.running = workers;
  try {
    for (size_t i = 0; i < workers; i++) pool.emplace_back(populate_worker, &work);
  } catch (...) {
    work.fail(ER_CANT_CREATE_THREAD, "cannot start a thread to fill the view");
    std::lock_guard<std::mutex> guard(work.mutex);
    work.running -= workers - pool.size();
  }
  {
    std::unique_lock<std::mutex> guard(work.mutex);
    while (work.running > 0) {
      work.changed.wait_for(guard, std::chrono::milliseconds(100));
      if (thd->killed && !work.stop) {
        work.stop = true;
        for (THD *worker : work.sessions) {
          mysql_mutex_lock(&worker->LOCK_thd_data);
          worker->awake(THD::KILL_QUERY);
          mysql_mutex_unlock(&worker->LOCK_thd_data);
        }
      }
    }
    work.decision = work.error == 0 && !thd->killed ? 1 : -1;
    work.changed.notify_all();
  }
  for (std::thread &worker : pool) worker.join();

  /* The statement has the view to itself again (REFRESH stores the
     definition, a failed CREATE drops the table). */
  const bool upgrade_failed = thd->mdl_context.upgrade_shared_lock(
      ticket, MDL_EXCLUSIVE, thd->variables.lock_wait_timeout);
  if (thd->killed) {
    thd->send_kill_message();
    return true;
  }
  if (work.error != 0) {
    my_message(work.error, work.message.c_str(), MYF(0));
    return true;
  }
  return upgrade_failed;
}

/**
  Empty the tables of the view and fill them from the base tables, on
  which the caller holds shared read-only metadata locks: the rows are
  exact as of the end of the binary log, which is recorded in the state
  table of an aggregate view.

  The rows are changed in the transaction of the statement, which the
  caller commits or rolls back (WARP empties with TRUNCATE, which commits:
  it does not reclaim the space of deleted rows otherwise).
*/
bool populate(Internal_session *session, const Mview_def &def, bool empty,
              Mview_locks *locks) {
  const bool truncate = is_warp(def.engine);
  if (!empty) {
    const std::string table = qualified(def.db, def.name);
    if (session->run(truncate ? "TRUNCATE TABLE " + table
                              : "DELETE FROM " + table))
      return true;
  }
  if (def.is_aggregate) {
    std::string file;
    ulonglong position = 0;
    if (mysql_bin_log.is_open()) {
      Log_info log_info;
      mysql_bin_log.get_current_log(&log_info);
      file = log_info.log_file_name + dirname_length(log_info.log_file_name);
      position = log_info.pos;
    }
    const std::string table = qualified(def.state_db, def.state_name);
    if (!empty && session->run(truncate ? "TRUNCATE TABLE " + table
                                        : "DELETE FROM " + table))
      return true;
    if (session->run("INSERT INTO " + table + " (binlog_file, binlog_pos) "
                     "VALUES (" + string_literal(file) + ", " +
                     std::to_string(position) + ")"))
      return true;
  }
  /* A join view that is empty (also after TRUNCATE, which committed) can
     be filled by several sessions; DELETE keeps its rows locked. */
  if (!def.is_aggregate && (empty || truncate)) {
    bool done = false;
    if (populate_parallel(session, def, locks, &done)) return true;
    if (done) return false;
  }
  return session->run(insert_statement(def));
}

/** The checks all the statements share. */
bool check_statement(THD *thd) {
  if (thd->locked_tables_mode) {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    return true;
  }
  return false;
}

bool not_a_mview(const char *db, const char *name) {
  my_error(ER_MVIEW_NOT_A_MVIEW, MYF(0), db, name);
  return true;
}

/** Shared read-only locks on the base tables of a view. */
void add_base_tables(const Mview_def &def, std::vector<Lock_request> *locks) {
  for (const Mview_table &mt : def.tables)
    add_once(locks, mt.db.c_str(), mt.table.c_str(), MDL_SHARED_READ_ONLY);
}

}  // namespace

/* ------------------------------------------------------------------ */
/* CREATE MATERIALIZED VIEW                                            */
/* ------------------------------------------------------------------ */

namespace {

/**
  Open the tables of the definition, resolve and check it, and build the
  metadata.  The tables are closed again when this returns.
*/
bool analyse_definition(THD *thd, Table_ref *view, bool warp, Mview_def *def,
                        Mview_build *build) {
  LEX *const lex = thd->lex;
  Query_block *const qb = lex->query_block;

  bool res = true;
  /*
    The resolver keeps the expressions printable for CREATE VIEW (see the
    uses of SQLCOM_CREATE_VIEW in the items), and so does it here.
  */
  const enum_sql_command saved_command = lex->sql_command;
  const uint8 saved_context = lex->context_analysis_only;
  lex->sql_command = SQLCOM_CREATE_VIEW;
  lex->context_analysis_only |= CONTEXT_ANALYSIS_ONLY_VIEW;

  if (open_temporary_tables(thd, lex->query_tables) ||
      open_tables_for_query(thd, lex->query_tables, 0))
    goto end;
  if (check_opened_tables(lex)) goto end;
  for (Table_ref *tr = lex->query_tables; tr != nullptr; tr = tr->next_global) {
    if (check_single_table_access(thd, SELECT_ACL, tr, false)) goto end;
  }

  if (lex->unit->prepare(thd, nullptr, nullptr, 0, 0)) goto end;

  {
    Mview_analysis analysis;
    if (check_resolved_query(qb, &analysis)) goto end;
    def->time_zone_dependent = analysis.time_zone_dependent;
    /* Print the way the internal statements are parsed: sql_mode=''. */
    const sql_mode_t saved_sql_mode = thd->variables.sql_mode;
    thd->variables.sql_mode = 0;
    res = build_tables(thd, qb, analysis.group_items, analysis.is_aggregate,
                       def) ||
          (analysis.is_aggregate
               ? build_aggregate_view(thd, view, qb, warp,
                                      analysis.group_items, def, build)
               : build_join_view(thd, view, qb, warp, def, build));
    thd->variables.sql_mode = saved_sql_mode;
    if (!res && build->column_definitions.size() > MAX_FIELDS) {
      my_error(ER_TOO_MANY_FIELDS, MYF(0));
      res = true;
    }
  }

end:
  lex->unit->cleanup(true);
  close_thread_tables(thd);
  lex->sql_command = saved_command;
  lex->context_analysis_only = saved_context;
  return res || thd->is_error();
}

bool create_mview(THD *thd, Table_ref *view) {
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  LEX *const lex = thd->lex;
  const char *db = view->db;
  const char *name = view->table_name;
  const bool if_not_exists =
      (lex->create_info->options & HA_LEX_CREATE_IF_NOT_EXISTS) != 0;

  if (check_statement(thd)) return true;

  if (is_infoschema_db(db) || is_perfschema_db(db) ||
      my_strcasecmp(system_charset_info, db, "mysql") == 0) {
    my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
             thd->security_context()->priv_user().str,
             thd->security_context()->priv_host().str, db);
    return true;
  }
  if (check_access(thd, CREATE_ACL, db, &view->grant.privilege,
                   &view->grant.m_internal, false, false) ||
      check_grant(thd, CREATE_ACL, view, false, 1, false))
    return true;

  if (check_parsed_query(lex)) return true;

  /* The engine: ENGINE=, or the default engine, as for CREATE TABLE. */
  handlerton *hton;
  if (lex->mview_engine.str != nullptr) {
    plugin_ref plugin = ha_resolve_by_name(thd, &lex->mview_engine, false);
    if (plugin == nullptr) {
      my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), lex->mview_engine.str);
      return true;
    }
    hton = plugin_data<handlerton *>(plugin);
  } else {
    hton = ha_default_handlerton(thd);
  }

  Mview_def def;
  Mview_build build;
  def.db = db;
  def.name = name;
  def.engine = ha_resolve_storage_engine_name(hton);
  def.refresh_mode = lex->mview_refresh_mode == 0
                         ? Mview_refresh_mode::MANUAL
                         : Mview_refresh_mode::SYNCHRONOUS;
  def.epoch = next_epoch(0);
  const bool warp = is_warp(def.engine);
  const std::string state_name = state_table_name(name);

  /*
    The view, its state table, and the base tables, which do not change
    until the view is filled.
  */
  Mview_locks locks(thd);
  {
    std::vector<Lock_request> requests;
    add_once(&requests, db, name, MDL_EXCLUSIVE);
    if (!state_name.empty())
      add_once(&requests, db, state_name.c_str(), MDL_EXCLUSIVE);
    for (Table_ref *tr = lex->query_tables; tr != nullptr;
         tr = tr->next_global)
      add_once(&requests, tr->db, tr->table_name, MDL_SHARED_READ_ONLY);
    if (locks.lock(requests)) return true;
  }

  bool schema_exists = false;
  if (dd::schema_exists(thd, db, &schema_exists)) return true;
  if (!schema_exists) {
    my_error(ER_BAD_DB_ERROR, MYF(0), db);
    return true;
  }
  {
    const dd::Abstract_table *existing = nullptr;
    if (thd->dd_client()->acquire(db, name, &existing)) return true;
    if (existing != nullptr) {
      if (!if_not_exists) {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), name);
        return true;
      }
      push_warning_printf(thd, Sql_condition::SL_NOTE, ER_TABLE_EXISTS_ERROR,
                          ER_THD(thd, ER_TABLE_EXISTS_ERROR), name);
      if (binlog_and_commit(thd)) return true;
      my_ok(thd);
      return false;
    }
  }

  if (analyse_definition(thd, view, warp, &def, &build)) return true;

  if (def.is_aggregate) {
    const dd::Abstract_table *existing = nullptr;
    if (thd->dd_client()->acquire(db, def.state_name.c_str(), &existing))
      return true;
    if (existing != nullptr) {
      my_error(ER_TABLE_EXISTS_ERROR, MYF(0), def.state_name.c_str());
      return true;
    }
  }

  /* Create the tables, mark them, fill them. */
  std::string columns;
  for (const std::string &column : build.column_definitions) {
    if (!columns.empty()) columns += ", ";
    columns += column;
  }
  std::string indexes;
  for (const std::string &index : build.index_definitions)
    indexes += ", " + index;
  const std::string table = qualified(db, name);
  const std::string create = "CREATE TABLE " + table + " (" + columns;
  const std::string options = ") ENGINE=" + def.engine;
  std::string drop = "DROP TABLE IF EXISTS " + table;

  bool failed;
  {
    Internal_session session(thd);
    /*
      Without the secondary indexes if the engine refuses them (the length
      of a key depends on the row format, for example).
    */
    failed = indexes.empty() ||
             session.run(create + indexes + options, false);
    if (failed) failed = session.run(create + options);
    if (failed) return true;

    if (def.is_aggregate) {
      const std::string state = qualified(db, def.state_name);
      drop += ", " + state;
      failed = session.run(
          "CREATE TABLE " + state +
          " (binlog_file varchar(255) CHARACTER SET utf8mb4 COLLATE "
          "utf8mb4_bin NULL, binlog_pos bigint unsigned NULL) ENGINE=" +
          def.engine);
    }
    if (!failed)
      failed = commit_or_rollback(
          thd, store_definition(thd, def, build.user_columns) ||
                   (def.is_aggregate &&
                    mark_table(thd, db, def.state_name.c_str(),
                               MVIEW_STATE_TABLE)));
    /* The shares of the tables do not know what the tables are. */
    tdc_remove_table(thd, TDC_RT_REMOVE_ALL, db, name, false);
    if (def.is_aggregate)
      tdc_remove_table(thd, TDC_RT_REMOVE_ALL, db, def.state_name.c_str(),
                       false);
    if (!failed) failed = populate(&session, def, true, &locks);
  }
  if (!failed) failed = binlog_and_commit(thd);
  if (failed) {
    commit_or_rollback(thd, true);
    /* Keep the error of the statement while the tables are dropped. */
    Diagnostics_area cleanup_da(false);
    thd->push_diagnostics_area(&cleanup_da);
    const THD::killed_state killed = thd->killed.load();
    thd->killed = THD::NOT_KILLED;
    {
      Internal_session session(thd);
      session.run(drop, false);
    }
    if (killed != THD::NOT_KILLED) thd->killed = killed;
    thd->pop_diagnostics_area();
    return true;
  }

  mview_registry_changed();
  my_ok(thd);
  return false;
}

}  // namespace

bool Sql_cmd_create_mview::execute(THD *thd) {
  LEX *const lex = thd->lex;
  bool link_to_local;
  /* The first table of the statement is the view. */
  Table_ref *view = lex->unlink_first_table(&link_to_local);
  const bool res = create_mview(thd, view);
  lex->link_first_table_back(view, link_to_local);
  return res;
}

/* ------------------------------------------------------------------ */
/* DROP MATERIALIZED VIEW                                              */
/* ------------------------------------------------------------------ */

bool Sql_cmd_drop_mview::execute(THD *thd) {
  Table_ref *const tables = thd->lex->query_tables;

  if (check_statement(thd)) return true;
  if (check_table_access(thd, DROP_ACL, tables, false, UINT_MAX, false))
    return true;

  Mview_locks locks(thd);
  {
    std::vector<Lock_request> requests;
    for (Table_ref *tr = tables; tr != nullptr; tr = tr->next_global) {
      add_once(&requests, tr->db, tr->table_name, MDL_EXCLUSIVE);
      const std::string state_name = state_table_name(tr->table_name);
      if (!state_name.empty())
        add_once(&requests, tr->db, state_name.c_str(), MDL_EXCLUSIVE);
    }
    if (locks.lock(requests)) return true;
  }

  /* All the names are checked before anything is dropped. */
  std::vector<std::string> drop_tables;
  for (Table_ref *tr = tables; tr != nullptr; tr = tr->next_global) {
    const dd::cache::Dictionary_client::Auto_releaser releaser(
        thd->dd_client());
    const dd::Abstract_table *table = nullptr;
    if (thd->dd_client()->acquire(tr->db, tr->table_name, &table)) return true;
    if (table == nullptr) {
      if (!m_if_exists) return not_a_mview(tr->db, tr->table_name);
      push_warning_printf(thd, Sql_condition::SL_NOTE, ER_MVIEW_NOT_A_MVIEW,
                          ER_THD(thd, ER_MVIEW_NOT_A_MVIEW), tr->db,
                          tr->table_name);
      continue;
    }
    if (mview_kind_from_dd(*table) != MVIEW_VIEW_TABLE)
      return not_a_mview(tr->db, tr->table_name);
    const std::string name = qualified(tr->db, tr->table_name);
    if (std::find(drop_tables.begin(), drop_tables.end(), name) !=
        drop_tables.end())
      continue;
    drop_tables.push_back(name);
    /* The state table of an aggregate view goes with it. */
    const std::string state_name = state_table_name(tr->table_name);
    const dd::Abstract_table *state = nullptr;
    if (!state_name.empty()) {
      if (thd->dd_client()->acquire(tr->db, state_name.c_str(), &state))
        return true;
      if (state != nullptr && mview_kind_from_dd(*state) == MVIEW_STATE_TABLE)
        drop_tables.push_back(qualified(tr->db, state_name));
    }
  }

  if (!drop_tables.empty()) {
    std::string drop_list;
    for (const std::string &name : drop_tables) {
      if (!drop_list.empty()) drop_list += ", ";
      drop_list += name;
    }
    Internal_session session(thd);
    if (session.run("DROP TABLE " + drop_list)) return true;
  }
  mview_registry_changed();
  if (binlog_and_commit(thd)) return true;
  my_ok(thd);
  return false;
}

/* ------------------------------------------------------------------ */
/* REFRESH MATERIALIZED VIEW                                           */
/* ------------------------------------------------------------------ */

bool Sql_cmd_refresh_mview::execute(THD *thd) {
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  Table_ref *const view = thd->lex->query_tables;
  const char *db = view->db;
  const char *name = view->table_name;

  if (check_statement(thd)) return true;
  if (check_table_access(thd, ALTER_ACL, view, false, UINT_MAX, false))
    return true;

  Mview_locks locks(thd);
  {
    std::vector<Lock_request> requests;
    add_once(&requests, db, name, MDL_EXCLUSIVE);
    const std::string state_name = state_table_name(name);
    if (!state_name.empty())
      add_once(&requests, db, state_name.c_str(), MDL_EXCLUSIVE);
    if (locks.lock(requests)) return true;
  }

  Mview_def def;
  std::vector<std::string> user_columns;
  bool exists, is_mview;
  if (load_definition(thd, db, name, &exists, &is_mview, &def, &user_columns))
    return true;
  if (!is_mview) return not_a_mview(db, name);

  /* The base tables do not change while the view is filled. */
  {
    std::vector<Lock_request> requests;
    add_base_tables(def, &requests);
    if (locks.lock(requests)) return true;
  }

  def.epoch = next_epoch(def.epoch);
  bool failed;
  {
    Internal_session session(thd);
    failed = populate(&session, def, false, &locks);
  }
  if (!failed) failed = store_definition(thd, def, user_columns);
  if (failed) {
    commit_or_rollback(thd, true);
    return true;
  }
  if (binlog_and_commit(thd)) return true;

  mview_registry_changed();
  my_ok(thd);
  return false;
}

/* ------------------------------------------------------------------ */
/* ALTER MATERIALIZED VIEW                                             */
/* ------------------------------------------------------------------ */

bool Sql_cmd_alter_mview::execute(THD *thd) {
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  Table_ref *const view = thd->lex->query_tables;
  const char *db = view->db;
  const char *name = view->table_name;

  if (check_statement(thd)) return true;
  if (check_table_access(thd, ALTER_ACL, view, false, UINT_MAX, false))
    return true;

  Mview_locks locks(thd);
  if (locks.lock({{db, name, MDL_EXCLUSIVE}})) return true;

  Mview_def def;
  std::vector<std::string> user_columns;
  bool exists, is_mview;
  if (load_definition(thd, db, name, &exists, &is_mview, &def, &user_columns))
    return true;
  if (!is_mview) return not_a_mview(db, name);

  def.refresh_mode = m_mode;
  def.epoch = next_epoch(def.epoch);
  if (store_definition(thd, def, user_columns)) {
    commit_or_rollback(thd, true);
    return true;
  }
  if (binlog_and_commit(thd)) return true;

  mview_registry_changed();
  my_ok(thd);
  return false;
}

/* ------------------------------------------------------------------ */
/* SHOW CREATE VIEW                                                    */
/* ------------------------------------------------------------------ */

bool mview_show_create(THD *thd, Table_ref *table_ref, String *buffer) {
  Mview_def def;
  std::vector<std::string> user_columns;
  bool exists, is_mview;
  if (load_definition(thd, table_ref->db, table_ref->table_name, &exists,
                      &is_mview, &def, &user_columns))
    return true;
  if (!is_mview) return not_a_mview(table_ref->db, table_ref->table_name);

  std::string sql = "CREATE MATERIALIZED VIEW ";
  if (thd->db().str == nullptr || strcmp(thd->db().str, def.db.c_str()) != 0)
    sql += quote(def.db) + ".";
  sql += quote(def.name);
  if (!user_columns.empty()) {
    sql += " (";
    for (size_t i = 0; i < user_columns.size(); i++) {
      if (i != 0) sql += ", ";
      sql += quote(user_columns[i]);
    }
    sql += ")";
  }
  sql += " ENGINE=" + def.engine;
  sql += def.refresh_mode == Mview_refresh_mode::SYNCHRONOUS
             ? " REFRESH SYNCHRONOUS"
             : " REFRESH MANUAL";
  sql += " AS " + def.definition;

  buffer->set_charset(sql_text_charset);
  buffer->length(0);
  return buffer->append(sql.data(), sql.size());
}
