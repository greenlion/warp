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
  @file storage/warp/warp_mview_daemon.cc

  The WARP_MVIEW daemon plugin: one background thread that reads the binary
  log of this server and keeps the SYNCHRONOUS materialized views (see
  sql/mview.h) up to date.

  Reader
  ------
  The thread reads the binary log files with Binlog_file_reader, never past
  MYSQL_BIN_LOG::get_binlog_end_pos() of the active file, and waits on the
  update condition of the binary log when it has caught up.  The end
  position is published when a commit group is flushed (or synced), which
  is BEFORE the storage engines commit.  Since the views are recomputed from
  the base tables, the thread must not look at the tables before those
  commits are done: after it has sampled the end position it acquires and
  releases the COMMIT metadata lock in shared mode (the lock FLUSH TABLES
  WITH READ LOCK uses to block commits; every committing transaction holds
  it from before the binary log write until after the engine commit).  When
  it is granted, every transaction up to the sampled position is committed
  in its engine.

  The changed rows of complete transactions are collected per base table, in
  binary log order, into a batch.  A batch is applied as soon as there is
  nothing more to read, or when it is big enough.

  Applying
  --------
  For every view, in one transaction of its own (the daemon's session does
  not write to the binary log):
    begin; apply the rows of the batch; [record the view's position];
    commit.
  Join views are recomputed by root key (see sql/mview.h); that is
  idempotent, so the batch may be replayed after a crash.  After all views
  are committed, the position of the reader is saved in the state file
  (datadir/warp_mview.state), which is the restart point.

  Aggregate views have one row per group and are changed by adding the
  deltas of the changed rows, or by computing the changed groups again; see
  "Aggregate views" below for the delta tables, the locking of the base
  tables ("pin") and the position each view records in its own transaction.

  Views the daemon has not followed continuously (new, recreated, changed
  from MANUAL, stale and refreshed by the user, or all of them when the
  saved position is lost) are "adopted" with a REFRESH MATERIALIZED VIEW,
  since rows may have changed between the snapshot of their contents and
  the position of the reader.
*/

#define MYSQL_SERVER 1
#define WARP_MVIEW_INTERNAL 1
#define LOG_COMPONENT_TAG "WARP_MVIEW"

#include "warp_mview.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <mysql/components/services/log_builtins.h>

#include "my_sys.h"
#include "my_thread.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "sql/binlog.h"
#include "sql/binlog/decompressing_event_object_istream.h"
#include "sql/binlog_index.h"
#include "sql/binlog_reader.h"
#include "sql/log_event.h"
#include "sql/mdl.h"
#include "sql/mview.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/protocol_classic.h"
#include "sql/sql_base.h"  // close_thread_tables
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/statement/ed_connection.h"
#include "sql/system_variables.h"
#include "sql/transaction.h"  // trans_commit_stmt

/* The error log service of this plugin (see log_builtins.h) */
static SERVICE_TYPE(registry) *reg_srv = nullptr;
SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

/* sql/sql_plugin.cc: held by INSTALL PLUGIN and UNINSTALL PLUGIN from start
   to end */
extern mysql_mutex_t LOCK_plugin_install;

namespace warp_mview {

using namespace mysql::binlog::event;

/* ------------------------------------------------------------------ */
/* System variables                                                    */
/* ------------------------------------------------------------------ */

static bool sv_enabled = true;
static bool sv_adopt_refresh = true;
static unsigned long sv_batch_rows = 10000;
static unsigned long sv_batch_transactions = 1000;
static unsigned long sv_in_list_chunk_size = 1000;
static unsigned long sv_full_refresh_threshold = 100000;
static unsigned long sv_idle_wait_timeout = 1000; /* milliseconds */
static unsigned long sv_lock_wait_timeout = 60;   /* seconds */
static unsigned long sv_retry_limit = 3;
static unsigned long sv_pin_lock_wait_timeout = 1; /* seconds */
static unsigned long sv_pin_duty_percent = 10;
static unsigned long sv_pin_min_interval = 0; /* milliseconds */

static MYSQL_SYSVAR_BOOL(
    enabled, sv_enabled, PLUGIN_VAR_OPCMDARG,
    "Maintain the SYNCHRONOUS materialized views from the binary log. "
    "OFF pauses the daemon; it continues from its saved position when "
    "turned ON again.",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_BOOL(
    adopt_refresh, sv_adopt_refresh, PLUGIN_VAR_OPCMDARG,
    "Run REFRESH MATERIALIZED VIEW once for every view the daemon has not "
    "followed continuously (a new or recreated view, a view changed to "
    "REFRESH SYNCHRONOUS, a stale view refreshed by the user).",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_ULONG(
    batch_rows, sv_batch_rows, PLUGIN_VAR_RQCMDARG,
    "Number of changed rows of the tracked base tables after which the "
    "transactions read so far are applied. A batch is always applied when "
    "there is nothing more to read.",
    nullptr, nullptr, 10000, 1, 100000000, 0);

static MYSQL_SYSVAR_ULONG(
    batch_transactions, sv_batch_transactions, PLUGIN_VAR_RQCMDARG,
    "Number of transactions of the binary log after which the transactions "
    "read so far are applied.",
    nullptr, nullptr, 1000, 1, 100000000, 0);

static MYSQL_SYSVAR_ULONG(
    in_list_chunk_size, sv_in_list_chunk_size, PLUGIN_VAR_RQCMDARG,
    "Number of keys in the IN list of one statement of the daemon.", nullptr,
    nullptr, 1000, 1, 100000, 0);

static MYSQL_SYSVAR_ULONG(
    full_refresh_threshold, sv_full_refresh_threshold, PLUGIN_VAR_RQCMDARG,
    "When one batch has more changed keys than this for a view, the view is "
    "rebuilt with REFRESH MATERIALIZED VIEW instead of key by key.",
    nullptr, nullptr, 100000, 1, ULONG_MAX, 0);

static MYSQL_SYSVAR_ULONG(
    idle_wait_timeout, sv_idle_wait_timeout, PLUGIN_VAR_RQCMDARG,
    "Milliseconds the daemon waits for the binary log to grow before it "
    "looks at warp_mview_enabled and at the materialized views again.",
    nullptr, nullptr, 1000, 10, 3600000, 0);

static MYSQL_SYSVAR_ULONG(
    lock_wait_timeout, sv_lock_wait_timeout,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "lock_wait_timeout, innodb_lock_wait_timeout and warp_lock_wait_timeout "
    "of the session of the daemon, in seconds. A statement that times out is "
    "retried.",
    nullptr, nullptr, 60, 1, 31536000, 0);

static MYSQL_SYSVAR_ULONG(
    retry_limit, sv_retry_limit, PLUGIN_VAR_RQCMDARG,
    "Number of times the changes of a view are applied again after an error "
    "that is not a lock wait timeout or a deadlock, before the view is "
    "rebuilt with REFRESH MATERIALIZED VIEW (and marked stale if that "
    "fails as well).",
    nullptr, nullptr, 3, 1, 1000, 0);

static MYSQL_SYSVAR_ULONG(
    pin_lock_wait_timeout, sv_pin_lock_wait_timeout, PLUGIN_VAR_RQCMDARG,
    "Seconds the daemon waits for LOCK TABLES ... READ on the base tables of "
    "aggregate materialized views that are computed from the base tables. "
    "New writers wait behind the request, so it is short; when it times out "
    "the daemon tries again later (see warp_mview_pin_duty_percent).",
    nullptr, nullptr, 1, 1, 31536000, 0);

static MYSQL_SYSVAR_ULONG(
    pin_duty_percent, sv_pin_duty_percent, PLUGIN_VAR_RQCMDARG,
    "The share of the time, in percent, that the daemon may spend waiting "
    "for and holding the read locks on the base tables of aggregate "
    "materialized views. Up to one second of this allowance is saved while "
    "the locks are not needed. The changes for the views that need the "
    "locks are collected meanwhile; other views are not delayed.",
    nullptr, nullptr, 10, 1, 100, 0);

static MYSQL_SYSVAR_ULONG(
    pin_min_interval, sv_pin_min_interval, PLUGIN_VAR_RQCMDARG,
    "Milliseconds between the end of one lock of the base tables of "
    "aggregate materialized views and the next lock request.",
    nullptr, nullptr, 0, 0, 3600000, 0);

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

static std::mutex status_mutex; /* protects the strings below */
static std::string st_state = "not started";
static std::string st_read_file;
static std::string st_applied_file;
static std::string st_stale_list;

static std::atomic<unsigned long long> st_read_pos{0};
static std::atomic<unsigned long long> st_applied_pos{0};
static std::atomic<unsigned long long> st_transactions{0};
static std::atomic<unsigned long long> st_batches{0};
static std::atomic<unsigned long long> st_rows{0};
static std::atomic<unsigned long long> st_keys{0};
static std::atomic<unsigned long long> st_apply_us{0};
static std::atomic<unsigned long long> st_statements{0};
static std::atomic<unsigned long long> st_pins{0};
static std::atomic<unsigned long long> st_pin_failures{0};
static std::atomic<unsigned long long> st_pin_lock_us{0};
static std::atomic<unsigned long long> st_pin_wait_us{0};
static std::atomic<unsigned long long> st_snapshots{0};
static std::atomic<unsigned long long> st_full_refreshes{0};
static std::atomic<unsigned long long> st_apply_errors{0};
static std::atomic<unsigned long long> st_stale_views{0};
static std::atomic<unsigned long long> st_views{0};
/* commit time (seconds) of the newest transaction read and of the newest
   one applied; equal when everything that was read is applied */
static std::atomic<long long> st_last_read_ts{0};
static std::atomic<long long> st_last_applied_ts{0};
static std::atomic<bool> st_caught_up{true};

static void set_state(const char *state) {
  std::lock_guard<std::mutex> guard(status_mutex);
  st_state = state;
}

static int show_string(const std::string &value, SHOW_VAR *var, char *buff) {
  std::lock_guard<std::mutex> guard(status_mutex);
  var->type = SHOW_CHAR;
  var->value = buff;
  size_t n = value.size();
  if (n > SHOW_VAR_FUNC_BUFF_SIZE - 1) n = SHOW_VAR_FUNC_BUFF_SIZE - 1;
  memcpy(buff, value.data(), n);
  buff[n] = 0;
  return 0;
}

static int show_number(unsigned long long value, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *reinterpret_cast<long long *>(buff) = static_cast<long long>(value);
  return 0;
}

static int show_state(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_string(st_state, var, buff);
}
static int show_read_file(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_string(st_read_file, var, buff);
}
static int show_applied_file(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_string(st_applied_file, var, buff);
}
static int show_stale_list(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_string(st_stale_list, var, buff);
}
static int show_read_pos(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_read_pos.load(), var, buff);
}
static int show_applied_pos(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_applied_pos.load(), var, buff);
}
static int show_transactions(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_transactions.load(), var, buff);
}
static int show_batches(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_batches.load(), var, buff);
}
static int show_rows(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_rows.load(), var, buff);
}
static int show_keys(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_keys.load(), var, buff);
}
static int show_apply_us(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_apply_us.load(), var, buff);
}
static int show_statements(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_statements.load(), var, buff);
}
static int show_pins(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_pins.load(), var, buff);
}
static int show_pin_failures(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_pin_failures.load(), var, buff);
}
static int show_pin_lock_us(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_pin_lock_us.load(), var, buff);
}
static int show_pin_wait_us(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_pin_wait_us.load(), var, buff);
}
static int show_snapshots(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_snapshots.load(), var, buff);
}
static int show_full_refreshes(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_full_refreshes.load(), var, buff);
}
static int show_apply_errors(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_apply_errors.load(), var, buff);
}
static int show_stale_views(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_stale_views.load(), var, buff);
}
static int show_views(MYSQL_THD, SHOW_VAR *var, char *buff) {
  return show_number(st_views.load(), var, buff);
}
static int show_lag(MYSQL_THD, SHOW_VAR *var, char *buff) {
  long long lag = 0;
  const long long read_ts = st_last_read_ts.load();
  const long long applied_ts = st_last_applied_ts.load();
  if (!st_caught_up.load() || read_ts != applied_ts) {
    /* the oldest change that is not applied is newer than the newest
       applied one */
    const long long base = applied_ts ? applied_ts : read_ts;
    if (base) lag = static_cast<long long>(time(nullptr)) - base;
    if (lag < 0) lag = 0;
  }
  return show_number(static_cast<unsigned long long>(lag), var, buff);
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void wmv_log(enum loglevel level, const char *format, ...)
    MY_ATTRIBUTE((format(printf, 2, 3)));

static void wmv_log(enum loglevel level, const char *format, ...) {
  char buf[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (log_bi != nullptr) LogPluginErr(level, ER_LOG_PRINTF_MSG, buf);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static std::atomic<bool> stop_requested{false};

/* The session of the thread, for warp_mview_plugin_deinit() to wake it */
static std::mutex session_mutex;
static THD *daemon_thds[2] = {nullptr, nullptr};

static bool server_going_down() {
  return connection_events_loop_aborted() ||
         get_server_state() == SERVER_SHUTTING_DOWN;
}

/* `name` */
static std::string qi(const std::string &name) {
  std::string out;
  out.reserve(name.size() + 2);
  out.push_back('`');
  for (char c : name) {
    if (c == '`') out.push_back('`');
    out.push_back(c);
  }
  out.push_back('`');
  return out;
}

static std::string qname(const std::string &db, const std::string &name) {
  return qi(db) + "." + qi(name);
}

/* The key of a base table in the maps: schema, 0 byte, table */
static std::string table_key(const std::string &db, const std::string &table) {
  std::string key = db;
  key.push_back('\0');
  key.append(table);
  return key;
}

static std::string printable_key(const std::string &key) {
  std::string out = key;
  for (char &c : out)
    if (c == '\0') c = '.';
  return out;
}

static std::string to_hex(const std::string &in) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(2 * in.size());
  for (unsigned char c : in) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
  }
  return out;
}

static bool from_hex(const std::string &in, std::string *out) {
  out->clear();
  if (in.size() % 2) return true;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < in.size(); i += 2) {
    const int hi = digit(in[i]), lo = digit(in[i + 1]);
    if (hi < 0 || lo < 0) return true;
    out->push_back(static_cast<char>(hi * 16 + lo));
  }
  return false;
}

/* FNV-1a, stable across restarts (it is stored in the state file) */
static uint64_t fnv1a(const std::string &data, uint64_t hash) {
  for (unsigned char c : data) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  hash ^= 0xff; /* separator */
  hash *= 1099511628211ULL;
  return hash;
}

static bool is_ident_char(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

/* Whether the statement has the identifier as a whole word, compared
   without case for ASCII letters. Conservative: a string or a comment that
   has the word matches as well. */
static bool mentions(const char *query, size_t length,
                     const std::string &ident) {
  const size_t n = ident.size();
  if (n == 0 || n > length) return false;
  for (size_t i = 0; i + n <= length; i++) {
    if (strncasecmp(query + i, ident.data(), n) != 0) continue;
    if (i > 0 && is_ident_char(static_cast<unsigned char>(query[i - 1])))
      continue;
    if (i + n < length &&
        is_ident_char(static_cast<unsigned char>(query[i + n])))
      continue;
    return true;
  }
  return false;
}

/* Whether the statement names the table `db`.`table`: it has the name of
   the table as a whole word, with the schema before it, or without a
   schema when the schema of the session that ran the statement
   (default_db) is the one of the table.  The names are compared without
   case for ASCII letters.

   Conservative as mentions() is, but not across schemas: a statement on a
   table of the same name in another schema (CREATE TABLE lineorder ... in
   a second copy of a schema) is not a statement on the table. */
static bool mentions_table(const char *query, size_t length,
                           const std::string &db, const std::string &table,
                           const std::string &default_db) {
  const size_t n = table.size();
  if (n == 0 || n > length) return false;
  const bool default_is_db =
      default_db.empty() || strcasecmp(default_db.c_str(), db.c_str()) == 0;
  for (size_t i = 0; i + n <= length; i++) {
    if (strncasecmp(query + i, table.data(), n) != 0) continue;
    if (i > 0 && is_ident_char(static_cast<unsigned char>(query[i - 1])))
      continue;
    if (i + n < length &&
        is_ident_char(static_cast<unsigned char>(query[i + n])))
      continue;
    /* what is before the name: [`]schema[`] [.] [`] */
    size_t at = i;
    if (at > 0 && query[at - 1] == '`') at--;
    while (at > 0 && (query[at - 1] == ' ' || query[at - 1] == '\t')) at--;
    if (at == 0 || query[at - 1] != '.') {
      if (default_is_db) return true;  // no schema: the one of the session
      continue;
    }
    at--;  // the dot
    while (at > 0 && (query[at - 1] == ' ' || query[at - 1] == '\t')) at--;
    size_t end = at;
    if (end > 0 && query[end - 1] == '`') {
      end--;
      size_t begin = end;
      while (begin > 0 && query[begin - 1] != '`') begin--;
      if (end - begin == db.size() &&
          strncasecmp(query + begin, db.data(), db.size()) == 0)
        return true;
    } else {
      size_t begin = end;
      while (begin > 0 &&
             is_ident_char(static_cast<unsigned char>(query[begin - 1])))
        begin--;
      if (end - begin == db.size() &&
          strncasecmp(query + begin, db.data(), db.size()) == 0)
        return true;
      /* nothing that is a name before the dot: not understood, the
         statement is taken for one on the table */
      if (end == begin) return true;
    }
  }
  return false;
}

/* The first words of a statement, upper case, without comments. `rest` is
   set to the text after the second word. */
static void leading_words(const char *query, size_t length, size_t max_words,
                          std::vector<std::string> *words, size_t *rest) {
  size_t i = 0;
  *rest = length;
  while (i < length && words->size() < max_words) {
    const unsigned char c = static_cast<unsigned char>(query[i]);
    if (c == '/' && i + 1 < length && query[i + 1] == '*') {
      i += 2;
      while (i + 1 < length && !(query[i] == '*' && query[i + 1] == '/')) i++;
      i = i + 2 <= length ? i + 2 : length;
    } else if (c == '#' || (c == '-' && i + 1 < length && query[i + 1] == '-')) {
      while (i < length && query[i] != '\n') i++;
    } else if (is_ident_char(c)) {
      std::string word;
      while (i < length &&
             is_ident_char(static_cast<unsigned char>(query[i]))) {
        word.push_back(static_cast<char>(
            toupper(static_cast<unsigned char>(query[i]))));
        i++;
      }
      words->push_back(std::move(word));
      if (words->size() == 2) *rest = i;
    } else {
      i++;
    }
  }
}

static std::string trimmed(const char *text, size_t length) {
  size_t b = 0, e = length;
  while (b < e && isspace(static_cast<unsigned char>(text[b]))) b++;
  while (e > b && (isspace(static_cast<unsigned char>(text[e - 1])) ||
                   text[e - 1] == ';'))
    e--;
  return std::string(text + b, e - b);
}

/* ------------------------------------------------------------------ */
/* The changes read from the binary log                                */
/* ------------------------------------------------------------------ */

/**
  Changed rows per base table, in binary log order.  The cells of a row are
  the wanted columns of the table (Tracked_table::columns).
*/
struct Table_changes {
  /// Which columns of the table the cells of the rows are (positions among
  /// the columns of the table, ascending).
  std::vector<unsigned> columns;
  std::vector<Row_change> rows;

  /// The cell position of a column of the table, -1 when it is not decoded.
  int slot(unsigned ordinal) const {
    for (size_t i = 0; i < columns.size(); i++)
      if (columns[i] == ordinal) return static_cast<int>(i);
    return -1;
  }
};

struct Changes {
  std::map<std::string, Table_changes> tables;
  /// Tables with changes that could not be decoded, or that a statement
  /// (DDL, or DML logged in statement format) changed: their views must be
  /// refreshed completely.
  std::set<std::string> refresh_tables;
  /// Changes that cannot be attributed to tables: refresh every view.
  bool refresh_all{false};
  size_t rows{0};

  bool empty() const {
    return tables.empty() && refresh_tables.empty() && !refresh_all;
  }
  void clear() {
    tables.clear();
    refresh_tables.clear();
    refresh_all = false;
    rows = 0;
  }
  /// Append the rows of a transaction, as transaction number `trx`.
  void merge(Changes *from, unsigned trx) {
    for (auto &entry : from->tables) {
      for (Row_change &row : entry.second.rows) row.trx = trx;
      Table_changes &to = tables[entry.first];
      if (to.rows.empty()) {
        to = std::move(entry.second);
      } else {
        to.rows.reserve(to.rows.size() + entry.second.rows.size());
        for (Row_change &row : entry.second.rows)
          to.rows.push_back(std::move(row));
      }
    }
    refresh_tables.insert(from->refresh_tables.begin(),
                          from->refresh_tables.end());
    refresh_all = refresh_all || from->refresh_all;
    rows += from->rows;
    from->clear();
  }
  /// The columns that are decoded for a table changed: move the cells.
  void remap(const std::string &key, const std::vector<unsigned> &columns) {
    auto entry = tables.find(key);
    if (entry == tables.end() || entry->second.columns == columns) return;
    Table_changes &t = entry->second;
    std::vector<int> from(columns.size());
    for (size_t i = 0; i < columns.size(); i++) from[i] = t.slot(columns[i]);
    for (Row_change &row : t.rows) {
      for (std::vector<Cell> *image : {&row.before, &row.after}) {
        if (image->empty()) continue;
        std::vector<Cell> cells(columns.size());
        for (size_t i = 0; i < columns.size(); i++)
          if (from[i] >= 0 && static_cast<size_t>(from[i]) < image->size())
            cells[i] = std::move((*image)[from[i]]);
        *image = std::move(cells);
      }
    }
    t.columns = columns;
  }
};

/** Complete transactions that are read and not applied. */
struct Batch {
  Changes changes;
  size_t transactions{0};
  /// The registry must be loaded again before the batch is applied (DDL).
  bool reload{false};
  /// Stop reading and apply now.
  bool close{false};
  /// Where the batch ends: a transaction boundary.
  std::string end_file;
  my_off_t end_pos{0};
  bool has_end{false};
  long long last_commit_ts{0};
  /// Where each transaction ends in the binary log (Row_change::trx).
  std::vector<std::pair<std::string, my_off_t>> trx_ends;

  bool has_work() const {
    return !changes.empty() || reload || transactions > 0;
  }
  /// Join views have the transactions before this one (Row_change::trx):
  /// the batch is applied in passes while it waits for a pin.
  unsigned join_applied_trx{0};
  /* identifies the batch across the attempts to apply it */
  uint64_t id{0};
  void clear() {
    static uint64_t next_id = 0;
    *this = Batch();
    id = ++next_id;
  }
};

/** A base table of at least one maintained view. */
struct Tracked_table {
  std::string db;
  std::string table;
  /// The columns that are decoded from the row images.
  std::vector<unsigned> columns;
};

/**
  The distinct old and new row keys (Mview_table::pk_columns) of the
  changed rows of one table, as SQL literals: "1" or "(1,'a')".  Keys with
  a NULL are kept apart: they are compared with <=>, not with IN.
*/
struct Key_sets {
  std::unordered_set<std::string> old_keys;
  std::unordered_set<std::string> new_keys;
  std::unordered_set<std::string> old_null_keys;
  std::unordered_set<std::string> new_null_keys;
  bool bad{false};
  std::string why;
  size_t size() const {
    return old_keys.size() + new_keys.size() + old_null_keys.size() +
           new_null_keys.size();
  }
};

/** The key sets of a table instance: views may have other row keys for
    the same table (the table changed between their CREATEs). */
static std::string key_set_id(const Mview_table &table) {
  std::string id = table.db;
  id.push_back('\0');
  id += table.table;
  id.push_back('\0');
  for (unsigned ordinal : table.pk_ordinals) id += std::to_string(ordinal) + ",";
  return id;
}

/** What the daemon remembers about a view between registry loads. */
struct Known_view {
  uint64_t epoch{0};
  uint64_t signature{0};
  bool stale{false};
};

/** A view of the registry and what is needed to maintain it. */
struct View_rt {
  std::string name;        ///< `db`.`name`: the table of the view
  std::string key_table;   ///< join views: the temporary table K
  std::vector<std::string> key_cols;  ///< the columns of K, quoted
  /// Join view with a WHERE: old rows that the WHERE filtered out are not
  /// in the view, so old images are mapped to root keys through the join
  /// (delta tables tmp_tables[1 + instance]).
  bool join_where{false};
  /// For join_where: the transactions of batch jw_batch before
  /// jw_applied_trx are in the view.
  uint64_t jw_batch{~0ULL};
  unsigned jw_applied_trx{0};
  uint64_t signature{0};
  bool valid{false};          ///< the definition can be used
  bool maintained{false};     ///< SYNCHRONOUS, valid, not stale
  bool needs_refresh{false};  ///< REFRESH before anything else
  std::string why_invalid;
  /// The temporary tables of the view in the session of the daemon.
  std::vector<std::string> tmp_tables;
  bool tmp_ready{false};

  /* aggregate views */
  std::string state_table;  ///< `state_db`.`state_name`
  /// The position up to which the view is exact (its state table).
  bool pos_valid{false};
  std::string pos_file;
  my_off_t pos{0};
  /// A MIN or MAX may have lost its value: apply with the base tables
  /// locked.
  bool force_pin{false};
  /// The view and all its base tables are InnoDB: its queries on the base
  /// tables read the snapshot of a transaction, so the tables are locked
  /// only while the snapshot is made.
  bool snapshot_class{false};
  /// The snapshot way failed: apply with the tables locked the next time.
  bool no_snapshot{false};
};

struct Sql_error {
  unsigned code{0};
  std::string message;
};

/* REPIN: apply the batch again with the base tables of aggregate views
   locked (see Daemon::acquire_pin) */
enum class Apply_result { OK, STOP, RESTART, FAILED, REPIN };

static PSI_stage_info stage_wmv_running = {0, "Maintaining materialized views",
                                           0, PSI_DOCUMENT_ME};
static PSI_stage_info stage_wmv_waiting = {
    0, "Waiting for the binary log to be updated", 0, PSI_DOCUMENT_ME};

/* ------------------------------------------------------------------ */
/* The daemon                                                          */
/* ------------------------------------------------------------------ */

class Daemon {
 public:
  void run(const void *stack_base);

 private:
  /* sessions */
  bool create_session(const void *stack_base);
  void destroy_session();
  void setup_session();
  bool exec_on(THD *session, const std::string &sql, Sql_error *error,
               std::vector<std::string> *row, std::vector<bool> *nulls);
  bool exec(const std::string &sql, Sql_error *error = nullptr,
            long long *int_result = nullptr);
  typedef std::vector<std::pair<bool, std::string>> Result_row;
  bool exec_rows(const std::string &sql, Sql_error *error,
                 std::vector<Result_row> *rows);
  bool copy_rows(const std::string &select, size_t columns,
                 const std::string &insert_head, Sql_error *error);
  void rollback();
  bool should_stop();
  void absorb_kill();
  void pause(unsigned long milliseconds);
  void log_limited(enum loglevel level, const std::string &key,
                   const std::string &message);
  bool commit_barrier();

  /* state file */
  std::string state_path() const;
  void load_state();
  bool save_state();
  void publish_stale();

  /* registry */
  bool load_registry();
  void drop_tmp_tables();
  bool view_exists(const std::string &db, const std::string &name,
                   bool *exists);
  void mark_stale(size_t view, const char *reason);
  void mark_in_sync(size_t view);

  /* reader */
  bool check_requirements();
  bool init_position();
  bool open_reader(const char *file, my_off_t pos, std::string *why);
  void close_reader();
  void lost_position(const char *why);
  enum class Read_result { BATCH, IDLE, FAILED, STOP };
  Read_result read_batch(std::string *why);
  void wait_for_binlog(my_off_t pos, unsigned long milliseconds);
  bool next_file(std::string *why);
  void process_event(Log_event *ev);
  void process_table_map(Table_map_log_event *ev);
  void process_rows(Rows_log_event *ev);
  void process_query(Query_log_event *ev);
  void add_transaction(Changes *changes, long long when);
  void commit_transaction(long long when);
  void discard_transaction();

  /* applier */
  Apply_result apply_batch(bool allow_defer);
  Apply_result apply_view(size_t view,
                          const std::map<std::string, Key_sets> &keys);
  Apply_result refresh_view(size_t view, const char *reason);
  bool apply_view_once(size_t view,
                       const std::map<std::string, Key_sets> &keys,
                       Sql_error *error, std::string *need_refresh);
  bool create_delta_table(const Mview_table &table, const std::string &name,
                          Sql_error *error);
  bool ensure_key_table(size_t view, Sql_error *error);
  bool insert_keys(const std::string &head, const std::string &tail,
                   const std::unordered_set<std::string> &keys, bool as_values,
                   bool single_column, Sql_error *error);
  bool insert_null_keys(const std::string &head, const std::string &lhs,
                        const std::unordered_set<std::string> &keys,
                        Sql_error *error);
  void make_key_sets(std::map<std::string, Key_sets> *keys, unsigned from_trx);
  /* Join views with a WHERE read their base tables to map old rows to
     root keys, so they are applied only with the base tables locked, all
     transactions of the batch since their last pinned apply at once. */
  unsigned join_where_start(size_t view) const {
    return rt[view].jw_batch == batch.id ? rt[view].jw_applied_trx : 0;
  }
  bool join_where_needs_pin(size_t view) const;
  void join_where_done(size_t view) {
    rt[view].jw_batch = batch.id;
    rt[view].jw_applied_trx = static_cast<unsigned>(batch.trx_ends.size());
  }
  /* the first transaction of the batch that apply_view_once() applies */
  unsigned join_from_trx{0};
  Apply_result after_error(const Sql_error &error, unsigned *attempt,
                           bool *give_up, const char *what,
                           const std::string &view);

  /* aggregate views */
  enum class Agg_outcome { DONE, NEED_PIN, NEED_REFRESH };
  void load_index_files();
  int file_ordinal(const std::string &file) const;
  bool row_is_after(const Row_change &row, int ordinal, my_off_t pos) const;
  bool read_view_position(size_t view, bool for_update, Sql_error *error);
  bool position_usable(size_t view) const;
  size_t rows_after_position(size_t view) const;
  bool reads_base_tables(size_t view) const;
  bool view_is_pinned(size_t view) const;
  void views_needing_pin(std::set<std::string> *tables);
  bool acquire_pin(const std::set<std::string> &tables, bool no_wait = false);
  void unlock_base_tables();
  bool lock_base_tables(const std::set<std::string> &tables, bool no_wait,
                        std::string *why);
  std::vector<MDL_ticket *> pin_tickets;
  void release_pin();
  void begin_snapshot();
  Apply_result apply_snapshot_group();
  void spend_pin_time(std::chrono::steady_clock::duration spent);
  bool pin_allowed(std::chrono::steady_clock::duration *wait);
  void wait_deferred(unsigned long milliseconds);
  std::string depends_mark() const;
  bool ensure_agg_tables(size_t view, Sql_error *error);
  bool fill_delta(size_t view, size_t instance, const std::string &delta,
                  bool old_only, int ordinal, my_off_t pos, unsigned from_trx,
                  size_t *rows, std::string *why_not, Sql_error *error);
  std::string agg_term(size_t view, const std::string &from,
                       const std::string &weight, bool single) const;
  bool apply_delta(size_t view, const std::vector<size_t> &changed,
                   Sql_error *error, Agg_outcome *outcome);
  bool apply_recompute(size_t view, const std::vector<size_t> &changed,
                       Sql_error *error, Agg_outcome *outcome);
  bool recompute_groups(size_t view, Sql_error *error);
  bool apply_agg_once(size_t view, Sql_error *error, Agg_outcome *outcome,
                      std::string *why, bool commit, bool *advanced);
  Apply_result apply_agg_view(size_t view);

  THD *thd{nullptr};       ///< the session that reads and writes the views
  THD *lock_thd{nullptr};  ///< the session that locks base tables (pin)

  /* state file contents */
  bool have_saved_position{false};
  std::string saved_file;
  my_off_t saved_pos{0};
  std::map<std::string, Known_view> known;  ///< by `db`.`name`
  std::map<std::string, std::pair<std::string, std::string>> known_names;

  /* registry */
  bool registry_loaded{false};
  uint64_t registry_version{0};
  std::vector<Mview_def> views;
  std::vector<View_rt> rt;
  std::unordered_map<std::string, Tracked_table> tracked;
  std::set<std::string> logged_once;
  /* the views of the registry before the last load: were they SYNCHRONOUS */
  std::map<std::string, bool> previous_views;
  bool ever_loaded{false};
  bool refresh_all_pending{false};

  /* reader */
  std::unique_ptr<Binlog_file_reader> reader;
  Log_info linfo;
  bool linfo_registered{false};
  bool positioned{false};
  bool limit_set{false};
  bool reading_active{false};
  my_off_t limit{0};
  unsigned read_failures{0};
  std::string requirement_logged;
  /* read exactly up to here (the position noted under the pin) */
  bool target_set{false};
  std::string target_file;
  my_off_t target_pos{0};

  /* the transaction being read */
  bool in_trx{false};
  Changes trx;
  std::string xa_xid;
  std::map<std::string, Changes> xa_prepared;
  std::unordered_map<unsigned long long, std::unique_ptr<Table_map_info>>
      table_maps;
  std::set<std::string> logged_undecodable;

  Batch batch;
  std::chrono::steady_clock::time_point last_state_save{};
  bool state_dirty{false};

  /* pin: base tables locked by lock_thd */
  bool pin_held{false};
  std::map<std::string,
           std::pair<std::chrono::steady_clock::time_point, unsigned long>>
      limited_logs;
  /* the statement that failed last, for the error log */
  std::string last_failed_sql;
  std::set<std::string> pinned_tables;
  /* the transaction of the working session has the snapshot in which the
     base tables of snapshot_views are read */
  bool snapshot_open{false};
  std::vector<size_t> snapshot_views;
  std::chrono::steady_clock::time_point pin_requested{};
  std::chrono::steady_clock::time_point pin_granted{};
  /* allowance of lock time (see warp_mview_pin_duty_percent) */
  double pin_budget_us{0};
  bool pin_budget_started{false};
  std::chrono::steady_clock::time_point pin_budget_time{};
  std::chrono::steady_clock::time_point pin_not_before{};
  /* what the last pass over the batch applied, and whether views wait
     for a pin */
  std::string partial_mark;
  bool batch_deferred{false};
  unsigned restarts{0};
  /* view -> depends_mark() of the batch during which it was refreshed */
  std::map<std::string, std::string> refreshed;
  /* the files of the binary log index, in order, without directories */
  std::vector<std::string> index_files;
  /* the transactions of the batch: position of their file in index_files */
  std::vector<int> trx_ordinals;
};

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static THD *make_session(const void *stack_base) {
  THD *session = new (std::nothrow) THD;
  if (session == nullptr) return nullptr;
  /* As the other background threads that run SQL (see the binlog thread
     of NDB): the thread id is needed before store_globals(). */
  session->set_new_thread_id();
  session->capture_stack_base(stack_base);
  session->store_globals();
  session->set_command(COM_DAEMON);
  session->system_thread = SYSTEM_THREAD_BACKGROUND;
  session->get_protocol_classic()->set_client_capabilities(0);
  session->security_context()->skip_grants();
  session->get_protocol_classic()->init_net(nullptr);
  session->init_query_mem_roots();
  lex_start(session);
  session->real_id = my_thread_self();
  session->set_time();
  session->enter_stage(&stage_wmv_running, nullptr, __func__, __FILE__,
                       __LINE__);
  /* In the list of sessions: it is killed (and waited for) at shutdown and
     shown by SHOW PROCESSLIST. */
  Global_THD_manager::get_instance()->add_thd(session);
  return session;
}

/* Two sessions that this thread uses in turn (the current one is switched
   by exec_on()): one for all the work, one that only holds LOCK TABLES. */
bool Daemon::create_session(const void *stack_base) {
  lock_thd = make_session(stack_base);
  thd = make_session(stack_base);
  if (thd == nullptr || lock_thd == nullptr) return true;
  std::lock_guard<std::mutex> guard(session_mutex);
  daemon_thds[0] = thd;
  daemon_thds[1] = lock_thd;
  return false;
}

void Daemon::destroy_session() {
  {
    std::lock_guard<std::mutex> guard(session_mutex);
    daemon_thds[0] = daemon_thds[1] = nullptr;
  }
  for (THD *session : {lock_thd, thd}) {
    if (session == nullptr) continue;
    session->store_globals();
    session->get_protocol_classic()->end_net();
    session->release_resources();
    Global_THD_manager::get_instance()->remove_thd(session);
    delete session;
  }
  thd = lock_thd = nullptr;
}

/* A message that may repeat: once a minute for the same key, with the
   number of times it was not written. */
void Daemon::log_limited(enum loglevel level, const std::string &key,
                         const std::string &message) {
  const auto now = std::chrono::steady_clock::now();
  auto &entry = limited_logs[key];
  if (entry.second != 0 && now - entry.first < std::chrono::seconds(60)) {
    entry.second++;
    return;
  }
  if (entry.second > 1)
    wmv_log(level, "%s (%lu more times in the last minute)", message.c_str(),
            entry.second - 1);
  else
    wmv_log(level, "%s", message.c_str());
  entry.first = now;
  entry.second = 1;
  if (limited_logs.size() > 1000) limited_logs.clear();
}

bool Daemon::should_stop() {
  if (stop_requested.load()) return true;
  if (!server_going_down()) return false;
  return (thd != nullptr && thd->killed) ||
         (lock_thd != nullptr && lock_thd->killed);
}

/* A KILL of the daemon by a user ends the statement it runs, not the
   daemon. */
void Daemon::absorb_kill() {
  if (stop_requested.load() || server_going_down()) return;
  for (THD *session : {thd, lock_thd}) {
    if (session == nullptr || !session->killed) continue;
    mysql_mutex_lock(&session->LOCK_thd_data);
    session->killed = THD::NOT_KILLED;
    mysql_mutex_unlock(&session->LOCK_thd_data);
    session->clear_error();
  }
}

void Daemon::pause(unsigned long milliseconds) {
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(milliseconds);
  while (!should_stop() && std::chrono::steady_clock::now() < until)
    my_sleep(20000); /* 20 ms */
}

/**
  Run one statement in one of the sessions of the daemon.

  @param[out] error  the error, if any
  @param[out] row    the columns of the first row of the result
  @param[out] nulls  which of them are NULL
  @retval false success
  @retval true  error
*/
bool Daemon::exec_on(THD *session, const std::string &sql, Sql_error *error,
                     std::vector<std::string> *row, std::vector<bool> *nulls) {
  if (current_thd != session) session->store_globals();
  st_statements.fetch_add(1);
  session->reset_for_next_command();
  session->set_query_id(next_query_id());
  session->set_time();

  Ed_connection connection(session);
  LEX_STRING text;
  text.str = const_cast<char *>(sql.c_str());
  text.length = sql.length();
  if (connection.execute_direct(text)) {
    if (session == thd) last_failed_sql = sql.substr(0, 600);
    if (error != nullptr) {
      error->code = connection.get_last_errno();
      const char *message = connection.get_last_error();
      error->message = message ? message : "";
      if (error->code == 0) error->code = ER_UNKNOWN_ERROR;
    }
    return true;
  }
  if (row != nullptr) {
    row->clear();
    if (nulls != nullptr) nulls->clear();
    Ed_result_set *result = connection.get_result_sets();
    if (result != nullptr && result->size() > 0) {
      List<Ed_row> &rows = *result;
      List_iterator_fast<Ed_row> it(rows);
      const Ed_row *first = it++;
      for (size_t i = 0; first != nullptr && i < first->size(); i++) {
        const Ed_column *column = first->get_column(i);
        const bool is_null = column->str == nullptr;
        row->push_back(is_null ? std::string()
                               : std::string(column->str, column->length));
        if (nulls != nullptr) nulls->push_back(is_null);
      }
    }
  }
  return false;
}

/* In the working session; int_result is the first column of the first row */
/* NOTE: Protocol_local returns integer and floating point columns as raw
   host-order bytes, not as text: every number that is read back must be
   selected as a string (CAST(... AS CHAR)). */
bool Daemon::exec(const std::string &sql, Sql_error *error,
                  long long *int_result) {
  if (int_result == nullptr) return exec_on(thd, sql, error, nullptr, nullptr);
  std::vector<std::string> row;
  *int_result = 0;
  if (exec_on(thd, sql, error, &row, nullptr)) return true;
  if (!row.empty()) *int_result = strtoll(row[0].c_str(), nullptr, 10);
  return false;
}

/* All rows of a result.  first: the value is NULL. */
bool Daemon::exec_rows(const std::string &sql, Sql_error *error,
                       std::vector<Result_row> *rows) {
  if (current_thd != thd) thd->store_globals();
  thd->reset_for_next_command();
  thd->set_query_id(next_query_id());
  thd->set_time();
  rows->clear();

  Ed_connection connection(thd);
  LEX_STRING text;
  text.str = const_cast<char *>(sql.c_str());
  text.length = sql.length();
  if (connection.execute_direct(text)) {
    last_failed_sql = sql.substr(0, 600);
    if (error != nullptr) {
      error->code = connection.get_last_errno();
      const char *message = connection.get_last_error();
      error->message = message ? message : "";
      if (error->code == 0) error->code = ER_UNKNOWN_ERROR;
    }
    return true;
  }
  Ed_result_set *result = connection.get_result_sets();
  if (result == nullptr) return false;
  List<Ed_row> &list = *result;
  List_iterator_fast<Ed_row> it(list);
  while (const Ed_row *row = it++) {
    Result_row out;
    out.reserve(row->size());
    for (size_t i = 0; i < row->size(); i++) {
      const Ed_column *column = row->get_column(i);
      if (column->str == nullptr)
        out.emplace_back(true, std::string());
      else
        out.emplace_back(false, std::string(column->str, column->length));
    }
    rows->push_back(std::move(out));
  }
  return false;
}

/**
  Run a SELECT and insert its rows with INSERT ... VALUES.

  This is how the results of queries on the base tables get into the
  temporary tables and the views: a plain SELECT is a consistent read of
  the snapshot of the transaction, the SELECT of an INSERT ... SELECT is
  not (InnoDB reads it with shared locks at REPEATABLE READ).  Every value
  travels as the hexadecimal form of its text (numbers and dates) or of its
  bytes (strings), so nothing has to be escaped and no result column is a
  number (see the note at exec()).

  @param select       the query; it must have `columns` columns
  @param insert_head  "INSERT INTO t (c, ...) VALUES "
*/
bool Daemon::copy_rows(const std::string &select, size_t columns,
                       const std::string &insert_head, Sql_error *error) {
  std::string sql = "SELECT ";
  std::string names;
  for (size_t i = 0; i < columns; i++) {
    const std::string name = "`mvc" + std::to_string(i) + "`";
    sql += std::string(i ? ", " : "") + "HEX(CAST(" + name + " AS BINARY))";
    names += std::string(i ? ", " : "") + name;
  }
  sql += " FROM (" + select + ") AS `mvx` (" + names + ")";
  std::vector<Result_row> rows;
  if (exec_rows(sql, error, &rows)) return true;

  const size_t chunk = sv_in_list_chunk_size ? sv_in_list_chunk_size : 1;
  std::string insert;
  size_t pending = 0;
  for (const Result_row &row : rows) {
    insert += pending ? ",(" : insert_head + "(";
    for (size_t i = 0; i < row.size(); i++) {
      if (i) insert.push_back(',');
      if (row[i].first)
        insert += "NULL";
      else
        insert += "_binary X'" + row[i].second + "'";
    }
    insert.push_back(')');
    if (++pending == chunk) {
      if (exec(insert, error)) return true;
      insert.clear();
      pending = 0;
    }
  }
  if (pending && exec(insert, error)) return true;
  return false;
}

void Daemon::rollback() {
  absorb_kill();
  Sql_error error;
  if (exec("ROLLBACK", &error) && !should_stop())
    wmv_log(WARNING_LEVEL, "ROLLBACK failed: %u %s", error.code,
            error.message.c_str());
}

void Daemon::setup_session() {
  const std::string timeout = std::to_string(sv_lock_wait_timeout);
  struct Setting {
    std::string sql;
    bool required;
  };
  const Setting settings[] = {
      {"SET NAMES utf8mb4", true},
      /* each server maintains its own views from its own binary log */
      {"SET SESSION sql_log_bin = 0", true},
      {"SET SESSION sql_mode = ''", true},
      /* The rows of the views are computed in UTC, as the statement that
         fills a view computes them (Internal_session in sql_mview.cc).  A
         TIMESTAMP of the binary log is written as FROM_UNIXTIME(seconds),
         the date and time of the time zone of the session: in a time zone
         with daylight saving time two points in time have the same one. */
      {"SET SESSION time_zone = '+00:00'", true},
      {"SET SESSION transaction_isolation = 'READ-COMMITTED'", true},
      {"SET SESSION autocommit = 0", true},
      {"SET SESSION lock_wait_timeout = " + timeout, true},
      {"SET SESSION innodb_lock_wait_timeout = " + timeout, false},
      {"SET SESSION warp_lock_wait_timeout = " + timeout, false},
      {"SET SESSION sql_require_primary_key = 0", false},
      {"SET SESSION sql_generate_invisible_primary_key = 0", false},
      {"COMMIT", true},
  };
  for (const Setting &setting : settings) {
    Sql_error error;
    if (exec(setting.sql, &error) && setting.required)
      wmv_log(ERROR_LEVEL, "'%s' failed: %u %s", setting.sql.c_str(),
              error.code, error.message.c_str());
  }
  mview_set_maintenance_mode(thd, true);
  for (const char *sql : {"SET NAMES utf8mb4", "SET SESSION sql_log_bin = 0",
                          "SET SESSION sql_mode = ''"})
    exec_on(lock_thd, sql, nullptr, nullptr, nullptr);
  thd->store_globals();
}

/**
  Wait until every transaction that is in the binary log now is committed
  in its storage engine (see the file comment).

  @retval false done
  @retval true  not acquired (timeout, killed); try again
*/
bool Daemon::commit_barrier() {
  if (current_thd != thd) thd->store_globals();
  MDL_request request;
  MDL_REQUEST_INIT(&request, MDL_key::COMMIT, "", "", MDL_SHARED,
                   MDL_EXPLICIT);
  /* A short timeout: commits wait behind this request. */
  if (thd->mdl_context.acquire_lock(&request, 2)) {
    thd->clear_error();
    return true;
  }
  thd->mdl_context.release_lock(request.ticket);
  return false;
}

/* ------------------------------------------------------------------ */
/* State file                                                          */
/* ------------------------------------------------------------------ */

/*
  datadir/warp_mview.state, text:

    warp_mview_state 1
    binlog <hex of the index entry of the file> <position>
    view <stale 0|1> <epoch> <signature> <hex schema> <hex name>
    ...
    end

  The position is a transaction boundary; everything before it is applied
  to every view that is listed and not stale.  Views that are not listed
  are adopted (refreshed) when they are seen.  Written to a temporary file,
  synced and renamed.
*/

std::string Daemon::state_path() const {
  std::string path = mysql_real_data_home;
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append("warp_mview.state");
  return path;
}

void Daemon::load_state() {
  have_saved_position = false;
  known.clear();
  known_names.clear();

  FILE *file = fopen(state_path().c_str(), "r");
  if (file == nullptr) {
    if (errno != ENOENT)
      wmv_log(ERROR_LEVEL, "cannot read %s: %s", state_path().c_str(),
              strerror(errno));
    return;
  }
  char line[4 * FN_REFLEN + 256];
  bool header = false, complete = false;
  std::string file_name;
  my_off_t pos = 0;
  bool have_pos = false;
  std::map<std::string, Known_view> views_read;
  std::map<std::string, std::pair<std::string, std::string>> names_read;

  while (fgets(line, sizeof(line), file) != nullptr) {
    char a[2 * FN_REFLEN + 8], b[2 * FN_REFLEN + 8];
    unsigned long long n1 = 0, n2 = 0;
    int flag = 0;
    if (!header) {
      if (strncmp(line, "warp_mview_state 1", 18) != 0) break;
      header = true;
    } else if (sscanf(line, "binlog %2055s %llu", a, &n1) == 2) {
      if (!from_hex(a, &file_name) && !file_name.empty()) {
        pos = n1;
        have_pos = true;
      }
    } else if (sscanf(line, "view %d %llu %llu %2055s %2055s", &flag, &n1,
                      &n2, a, b) == 5) {
      std::string db, name;
      if (!from_hex(a, &db) && !from_hex(b, &name)) {
        Known_view view;
        view.stale = flag != 0;
        view.epoch = n1;
        view.signature = n2;
        const std::string key = qname(db, name);
        views_read[key] = view;
        names_read[key] = std::make_pair(db, name);
      }
    } else if (strncmp(line, "end", 3) == 0) {
      complete = true;
      break;
    }
  }
  fclose(file);

  if (!header || !complete || !have_pos) {
    wmv_log(ERROR_LEVEL, "%s is damaged and ignored", state_path().c_str());
    return;
  }
  have_saved_position = true;
  saved_file = file_name;
  saved_pos = pos;
  mview_set_applied_position(saved_file, saved_pos);
  known = std::move(views_read);
  known_names = std::move(names_read);
  publish_stale();
}

bool Daemon::save_state() {
  if (!have_saved_position) return false;
  std::string text = "warp_mview_state 1\n";
  text += "binlog " + to_hex(saved_file) + " " + std::to_string(saved_pos) +
          "\n";
  for (const auto &entry : known) {
    auto names = known_names.find(entry.first);
    if (names == known_names.end()) continue;
    text += "view " + std::string(entry.second.stale ? "1" : "0") + " " +
            std::to_string(entry.second.epoch) + " " +
            std::to_string(entry.second.signature) + " " +
            to_hex(names->second.first) + " " + to_hex(names->second.second) +
            "\n";
  }
  text += "end\n";

  const std::string path = state_path();
  const std::string tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
  bool failed = fd < 0;
  if (!failed) {
    size_t done = 0;
    while (done < text.size()) {
      const ssize_t n = write(fd, text.data() + done, text.size() - done);
      if (n < 0) {
        if (errno == EINTR) continue;
        failed = true;
        break;
      }
      done += static_cast<size_t>(n);
    }
    if (!failed && fsync(fd) != 0) failed = true;
    if (close(fd) != 0) failed = true;
  }
  if (!failed && rename(tmp.c_str(), path.c_str()) != 0) failed = true;
  if (failed) {
    wmv_log(ERROR_LEVEL, "cannot write %s: %s", path.c_str(), strerror(errno));
    return true;
  }
  /* make the rename durable */
  int dir = open(mysql_real_data_home, O_RDONLY | O_CLOEXEC);
  if (dir >= 0) {
    (void)fsync(dir);
    (void)close(dir);
  }
  {
    std::lock_guard<std::mutex> guard(status_mutex);
    st_applied_file = saved_file;
  }
  st_applied_pos.store(saved_pos);
  return false;
}

void Daemon::publish_stale() {
  std::string list;
  unsigned long long count = 0;
  for (const auto &entry : known) {
    if (!entry.second.stale) continue;
    count++;
    if (!list.empty()) list.push_back(',');
    auto names = known_names.find(entry.first);
    if (names != known_names.end())
      list += names->second.first + "." + names->second.second;
    else
      list += entry.first;
  }
  st_stale_views.store(count);
  {
    std::lock_guard<std::mutex> guard(status_mutex);
    st_stale_list = list;
  }

  /* the views that are current, for the query rewrite plugin */
  std::vector<Mview_maintained> current;
  for (size_t i = 0; i < views.size() && i < rt.size(); i++) {
    if (!rt[i].maintained || rt[i].needs_refresh) continue;
    current.push_back({views[i].db, views[i].name, views[i].epoch});
  }
  mview_set_maintained(current);
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

static bool is_transient(unsigned code) {
  switch (code) {
    case ER_LOCK_WAIT_TIMEOUT:
    case ER_LOCK_DEADLOCK:
    case ER_QUERY_INTERRUPTED:
    case ER_QUERY_TIMEOUT:
    case ER_CANT_UPDATE_WITH_READLOCK:
    case ER_OPTION_PREVENTS_STATEMENT: /* read_only */
    case ER_LOCK_ABORTED:
    case ER_XA_RBTIMEOUT:
    case ER_XA_RBDEADLOCK:
    case ER_TABLE_DEF_CHANGED:
    case ER_NEED_REPREPARE:
    case ER_SERVER_SHUTDOWN:
      return true;
    default:
      return false;
  }
}

static uint64_t view_signature(const Mview_def &def) {
  uint64_t hash = 14695981039346656037ULL;
  hash = fnv1a(def.definition, hash);
  hash = fnv1a(def.engine, hash);
  hash = fnv1a(def.is_aggregate ? "a" : "j", hash);
  hash = fnv1a(std::to_string(def.root), hash);
  hash = fnv1a(def.from_template, hash);
  hash = fnv1a(def.where_sql, hash);
  for (const Mview_table &table : def.tables) {
    hash = fnv1a(table.db, hash);
    hash = fnv1a(table.table, hash);
    hash = fnv1a(table.alias, hash);
    for (const std::string &column : table.pk_columns)
      hash = fnv1a(column, hash);
    for (const std::string &column : table.key_columns)
      hash = fnv1a(column, hash);
    for (const std::string &column : table.ref_columns)
      hash = fnv1a(column, hash);
  }
  for (const std::string &expr : def.group_exprs) hash = fnv1a(expr, hash);
  for (const Mview_agg_state &state : def.states) {
    hash = fnv1a(state.column, hash);
    hash = fnv1a(state.full_sql, hash);
  }
  return hash;
}

static bool definition_usable(const Mview_def &def, std::string *why) {
  if (def.tables.empty()) {
    *why = "it has no base tables";
    return false;
  }
  if (def.from_sql.empty()) {
    *why = "its definition is incomplete";
    return false;
  }
  for (const Mview_table &table : def.tables) {
    if (table.pk_columns.size() != table.pk_ordinals.size()) {
      *why = "the key columns of " + table.db + "." + table.table +
             " are inconsistent";
      return false;
    }
  }
  if (!def.is_aggregate) {
    if (def.root >= def.tables.size()) {
      *why = "its root table is out of range";
      return false;
    }
    for (const Mview_table &table : def.tables) {
      /* an empty row key: every change of the table refreshes the view */
      if (table.pk_columns.size() != table.key_columns.size()) {
        *why = "the key columns of " + table.db + "." + table.table +
               " are inconsistent";
        return false;
      }
    }
    if (def.detail_columns.empty() || def.detail_select.empty()) {
      *why = "its definition is incomplete";
      return false;
    }
    if (!def.where_sql.empty()) {
      for (size_t t = 0; t < def.tables.size(); t++) {
        const Mview_table &table = def.tables[t];
        if (table.ref_columns.empty() ||
            table.ref_columns.size() != table.ref_ordinals.size() ||
            def.from_template.find("`mv$source$" + std::to_string(t) + "`") ==
                std::string::npos) {
          *why = "it has a WHERE and its definition lacks the referenced "
                 "columns or the FROM template";
          return false;
        }
      }
    }
    return true;
  }
  if (def.from_template.empty() || def.count_column.empty() ||
      def.state_name.empty() ||
      def.group_exprs.size() != def.view_group_columns.size()) {
    *why = "its definition is incomplete";
    return false;
  }
  for (size_t t = 0; t < def.tables.size(); t++) {
    const Mview_table &table = def.tables[t];
    /* A view may read no column of a table (SELECT COUNT(*) FROM t): the
       rows of the table are counted, their delta table only has the sign. */
    if (table.ref_columns.size() != table.ref_ordinals.size()) {
      *why = "the referenced columns of " + table.db + "." + table.table +
             " are inconsistent";
      return false;
    }
    if (def.from_template.find("`mv$source$" + std::to_string(t) + "`") ==
        std::string::npos) {
      *why = "its FROM template does not have all tables";
      return false;
    }
  }
  for (const Mview_agg_state &state : def.states) {
    if (state.column.empty() || state.full_sql.empty() ||
        (state.kind != Mview_agg_state::RECOMPUTE &&
         state.kind != Mview_agg_state::COUNT_STAR && state.arg_sql.empty())) {
      *why = "an aggregate state is incomplete";
      return false;
    }
    if (def.delta_capable && state.kind == Mview_agg_state::RECOMPUTE) {
      *why = "an aggregate state cannot be maintained with deltas";
      return false;
    }
    if (state.kind == Mview_agg_state::SUM &&
        (state.count_state < 0 ||
         static_cast<size_t>(state.count_state) >= def.states.size() ||
         def.states[state.count_state].kind != Mview_agg_state::COUNT)) {
      *why = "a SUM state has no COUNT state";
      return false;
    }
  }
  return true;
}

void Daemon::drop_tmp_tables() {
  bool any = false;
  for (View_rt &view : rt) {
    if (!view.tmp_ready && view.tmp_tables.empty()) continue;
    for (const std::string &table : view.tmp_tables)
      exec("DROP TEMPORARY TABLE IF EXISTS " + table);
    view.tmp_ready = false;
    any = true;
  }
  if (any) exec("COMMIT");
}

/* Whether the table of a view exists (to tell a dropped view from one
   that cannot be loaded) */
bool Daemon::view_exists(const std::string &db, const std::string &name,
                         bool *exists) {
  long long count = 0;
  Sql_error error;
  const bool failed = exec(
      "SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.tables WHERE table_schema = "
      "CONVERT(_utf8mb4 X'" + to_hex(db) + "' USING utf8mb3) AND table_name = "
      "CONVERT(_utf8mb4 X'" + to_hex(name) + "' USING utf8mb3)",
      &error, &count);
  exec("COMMIT");
  if (failed) {
    wmv_log(WARNING_LEVEL, "cannot look up %s.%s: %u %s", db.c_str(),
            name.c_str(), error.code, error.message.c_str());
    return true;
  }
  *exists = count > 0;
  return false;
}

void Daemon::mark_stale(size_t view, const char *reason) {
  Known_view &entry = known[rt[view].name];
  entry.epoch = views[view].epoch;
  entry.signature = rt[view].signature;
  entry.stale = true;
  known_names[rt[view].name] = std::make_pair(views[view].db, views[view].name);
  rt[view].maintained = false;
  rt[view].needs_refresh = false;
  wmv_log(ERROR_LEVEL,
          "materialized view %s is STALE and no longer maintained: %s. Run "
          "REFRESH MATERIALIZED VIEW to make it current.",
          rt[view].name.c_str(), reason);
  publish_stale();
}

void Daemon::mark_in_sync(size_t view) {
  Known_view &entry = known[rt[view].name];
  entry.epoch = views[view].epoch;
  entry.signature = rt[view].signature;
  entry.stale = false;
  known_names[rt[view].name] = std::make_pair(views[view].db, views[view].name);
  rt[view].maintained = true;
  rt[view].needs_refresh = false;
  publish_stale();
}

/**
  Load the materialized views and decide which ones are maintained.

  @retval false done
  @retval true  the registry cannot be read now; try again
*/
bool Daemon::load_registry() {
  absorb_kill();
  if (current_thd != thd) thd->store_globals();
  /* read first: a change during the load is seen as a new version later */
  const uint64_t version = mview_registry_version.load();

  std::vector<Mview_def> loaded;
  thd->reset_for_next_command();
  thd->set_query_id(next_query_id());
  thd->set_time();
  mview_set_maintenance_mode(thd, true);
  const bool failed = mview_load_all(thd, &loaded);
  std::string message;
  unsigned code = 0;
  if (failed && thd->is_error()) {
    code = thd->get_stmt_da()->mysql_errno();
    message = thd->get_stmt_da()->message_text();
  }
  /* what the end of a statement does, in case the loader left anything */
  if (failed)
    trans_rollback_stmt(thd);
  else
    trans_commit_stmt(thd);
  close_thread_tables(thd);
  thd->clear_error();
  exec(failed ? "ROLLBACK" : "COMMIT");
  if (failed) {
    if (!should_stop())
      wmv_log(ERROR_LEVEL, "cannot load the materialized views: %u %s", code,
              message.c_str());
    return true;
  }

  drop_tmp_tables();
  registry_loaded = false; /* until the end of this function */
  previous_views.clear();
  for (const Mview_def &def : views)
    previous_views[qname(def.db, def.name)] =
        def.refresh_mode == Mview_refresh_mode::SYNCHRONOUS;
  views = std::move(loaded);
  rt.assign(views.size(), View_rt());
  tracked.clear();
  table_maps.clear();

  /* where the reader is: views that are exact as of an earlier position
     and were not followed have a gap */
  load_index_files();
  const int reader_ordinal = reader ? file_ordinal(linfo.log_file_name) : -1;
  const my_off_t reader_pos = reader ? reader->position() : 0;
  std::unordered_map<std::string, std::set<unsigned>> columns;

  std::set<std::string> seen;
  unsigned long long maintained_count = 0;
  for (size_t i = 0; i < views.size(); i++) {
    const Mview_def &def = views[i];
    View_rt &view = rt[i];
    view.name = qname(def.db, def.name);
    view.signature = view_signature(def);
    seen.insert(view.name);

    if (def.refresh_mode != Mview_refresh_mode::SYNCHRONOUS) {
      /* not followed: adopted if it becomes SYNCHRONOUS */
      known.erase(view.name);
      known_names.erase(view.name);
      continue;
    }
    known_names[view.name] = std::make_pair(def.db, def.name);
    view.valid = definition_usable(def, &view.why_invalid);
    auto entry = known.find(view.name);
    if (!view.valid) {
      if (entry == known.end() || !entry->second.stale ||
          entry->second.epoch != def.epoch)
        mark_stale(i, view.why_invalid.c_str());
      continue;
    }

    const std::string n = std::to_string(i);
    if (def.is_aggregate) {
      view.state_table = qname(def.state_db.empty() ? def.db : def.state_db,
                               def.state_name);
      for (size_t t = 0; t < def.tables.size(); t++)
        view.tmp_tables.push_back(
            qname(def.db, "warp_mview_d" + n + "_" + std::to_string(t)));
      for (const char *kind : {"gd", "gs", "nv", "rg"})
        view.tmp_tables.push_back(qname(def.db, "warp_mview_" + std::string(kind) + n));
    } else {
      view.key_table = qname(def.db, "warp_mview_k" + n);
      view.tmp_tables.push_back(view.key_table);
      view.join_where = !def.where_sql.empty();
      if (view.join_where)
        for (size_t t = 0; t < def.tables.size(); t++)
          view.tmp_tables.push_back(
              qname(def.db, "warp_mview_d" + n + "_" + std::to_string(t)));
      for (size_t k = 0; k < def.tables[def.root].pk_columns.size(); k++)
        view.key_cols.push_back(qi("warp_mvk" + std::to_string(k)));
    }

    /*
      Does the view have every change that the reader is past?

      CREATE and REFRESH MATERIALIZED VIEW hold read-only locks on the base
      tables from before they fill the view until after they have
      announced the change of the registry, and the reader looks at the
      registry after it has read the first event of every transaction
      (read_batch).  So when a view appears, or is filled again, the
      changes that were read before are in it, and the later ones are
      read with the view known: nothing is missing and no refresh is
      needed.

      A refresh is needed ("adopt") for a view that this daemon saw as
      REFRESH MANUAL (ALTER does not fill it), and for a view that is not
      in the state file when the daemon starts: what happened to it while
      the daemon did not run is not known.
    */
    bool adopt;
    auto before = previous_views.find(view.name);
    if (entry == known.end()) {
      adopt = !ever_loaded ||
              (before != previous_views.end() && !before->second);
    } else if (entry->second.stale) {
      /* stale until the user refreshes or recreates it */
      if (entry->second.epoch == def.epoch &&
          entry->second.signature == view.signature)
        continue;
      adopt = false;
      entry->second.stale = false;
    } else {
      adopt = false; /* followed without a gap, or made again */
    }
    if (entry != known.end()) {
      entry->second.epoch = def.epoch;
      entry->second.signature = view.signature;
    }
    if (refresh_all_pending) adopt = true;

    if (def.is_aggregate) {
      /* An aggregate view says itself up to where it is exact.  It needs a
         refresh when that is unknown, or when the reader is past it and
         did not collect the rows of the view on the way. */
      Sql_error error;
      if (read_view_position(i, false, &error)) {
        exec("ROLLBACK");
        if (is_transient(error.code) || should_stop()) return true;
        if (error.code == ER_NO_SUCH_TABLE) {
          /* The view is being dropped (its state table goes first) or
             created: it is looked at again when the registry changes. */
          view.valid = false;
          log_limited(INFORMATION_LEVEL, view.name + " state",
                      "materialized view " + view.name +
                          " has no state table (yet, or any more); it is "
                          "not maintained until the views change");
          continue;
        }
        mark_stale(i, ("its state table cannot be read: " + error.message)
                          .c_str());
        continue;
      }
      exec("COMMIT");
      const bool usable = position_usable(i);
      bool behind = false;
      if (usable && adopt) {
        const int ordinal = file_ordinal(view.pos_file);
        behind = ordinal < reader_ordinal ||
                 (ordinal == reader_ordinal && view.pos < reader_pos);
      }
      if (usable && !behind && !refresh_all_pending) {
        mark_in_sync(i);
        adopt = false;
      } else {
        adopt = true;
      }
    } else if (!adopt || (!sv_adopt_refresh && !refresh_all_pending)) {
      mark_in_sync(i);
      adopt = false;
    }
    if (def.is_aggregate && strcasecmp(def.engine.c_str(), "InnoDB") == 0) {
      /* are all base tables InnoDB? */
      std::string sql =
          "SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.tables "
          "WHERE engine = 'InnoDB' AND (FALSE";
      std::set<std::string> distinct;
      for (const Mview_table &table : def.tables) {
        if (!distinct.insert(table_key(table.db, table.table)).second)
          continue;
        sql += " OR (table_schema = CONVERT(_utf8mb4 X'" + to_hex(table.db) +
               "' USING utf8mb3) AND table_name = CONVERT(_utf8mb4 X'" +
               to_hex(table.table) + "' USING utf8mb3))";
      }
      sql += ")";
      long long count = 0;
      if (!exec(sql, nullptr, &count))
        view.snapshot_class = count == static_cast<long long>(distinct.size());
      exec("COMMIT");
    }
    view.maintained = !adopt;
    view.needs_refresh = adopt;
    if (adopt) {
      /* not in the state file until it is refreshed */
      known.erase(view.name);
    }
    maintained_count++;

    for (const Mview_table &table : def.tables) {
      const std::string key = table_key(table.db, table.table);
      Tracked_table &t = tracked[key];
      t.db = table.db;
      t.table = table.table;
      /* Join views need the row key, aggregate views every column they
         reference. */
      std::set<unsigned> &wanted = columns[key];
      wanted.insert(table.pk_ordinals.begin(), table.pk_ordinals.end());
      if (def.is_aggregate || view.join_where)
        wanted.insert(table.ref_ordinals.begin(), table.ref_ordinals.end());
    }
  }
  for (auto &entry : tracked) {
    std::set<unsigned> &wanted = columns[entry.first];
    /* rows that are read and not applied keep their columns */
    auto pending = batch.changes.tables.find(entry.first);
    if (pending != batch.changes.tables.end())
      wanted.insert(pending->second.columns.begin(),
                    pending->second.columns.end());
    entry.second.columns.assign(wanted.begin(), wanted.end());
    batch.changes.remap(entry.first, entry.second.columns);
    for (auto &prepared : xa_prepared)
      prepared.second.remap(entry.first, entry.second.columns);
  }
  refresh_all_pending = false;

  /* Views that were followed and are not in the registry any more: dropped,
     or they cannot be loaded (e.g. a base table was dropped). */
  for (auto it = known.begin(); it != known.end();) {
    if (seen.count(it->first)) {
      ++it;
      continue;
    }
    auto names = known_names.find(it->first);
    bool exists = false;
    if (names == known_names.end() ||
        (!view_exists(names->second.first, names->second.second, &exists) &&
         !exists)) {
      known_names.erase(it->first);
      it = known.erase(it);
      continue;
    }
    if (!it->second.stale) {
      it->second.stale = true;
      wmv_log(ERROR_LEVEL,
              "materialized view %s cannot be loaded any more; it is STALE "
              "and no longer maintained",
              it->first.c_str());
    }
    ++it;
  }
  /* names of views that are neither known nor in the registry */
  for (auto it = known_names.begin(); it != known_names.end();) {
    if (known.count(it->first) || seen.count(it->first))
      ++it;
    else
      it = known_names.erase(it);
  }

  publish_stale();
  st_views.store(maintained_count);
  registry_version = version;
  registry_loaded = true;
  ever_loaded = true;
  return false;
}

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

/* The binary log must be enabled, in row format. */
bool Daemon::check_requirements() {
  const char *problem = nullptr;
  if (!opt_bin_log || !mysql_bin_log.is_open())
    problem = "the binary log is not enabled (log_bin)";
  else if (global_system_variables.binlog_format != BINLOG_FORMAT_ROW)
    problem = "binlog_format is not ROW";
  if (problem == nullptr) {
    if (!requirement_logged.empty()) {
      wmv_log(INFORMATION_LEVEL,
              "the binary log is usable again; maintaining materialized "
              "views");
      requirement_logged.clear();
    }
    return false;
  }
  if (requirement_logged != problem) {
    wmv_log(ERROR_LEVEL,
            "SYNCHRONOUS materialized views are not maintained: %s", problem);
    requirement_logged = problem;
  }
  set_state((std::string("disabled: ") + problem).c_str());
  return true;
}

void Daemon::close_reader() {
  reader.reset();
  limit_set = false;
  positioned = false;
}

/**
  Open a binary log file at a position.

  @param file  the name as it is in the index file; nullptr for the first
*/
bool Daemon::open_reader(const char *file, my_off_t pos, std::string *why) {
  reader.reset();
  limit_set = false;
  if (!linfo_registered) {
    /* Registered like a dump thread: the file being read is not purged. */
    linfo.thread_id = thd->thread_id();
    mysql_bin_log.register_log_info(&linfo);
    linfo_registered = true;
  }
  if (mysql_bin_log.find_log_pos(&linfo, file, true /* lock the index */)) {
    *why = std::string("binary log file ") + (file ? file : "") +
           " is not in the index (purged?)";
    return true;
  }
  reader.reset(new Binlog_file_reader(true /* verify checksums */));
  if (pos < BIN_LOG_HEADER_SIZE) pos = BIN_LOG_HEADER_SIZE;
  if (reader->open(linfo.log_file_name, pos)) {
    *why = std::string("cannot open ") + linfo.log_file_name + ": " +
           reader->get_error_str();
    reader.reset();
    return true;
  }
  if (reader->position() != pos || reader->ifile()->length() < pos) {
    *why = std::string("position ") + std::to_string(pos) + " is not in " +
           linfo.log_file_name;
    reader.reset();
    return true;
  }
  {
    std::lock_guard<std::mutex> guard(status_mutex);
    st_read_file = linfo.log_file_name;
  }
  st_read_pos.store(pos);
  return false;
}

/* The saved position cannot be used: every view is refreshed and the
   reader continues at the end of the binary log. */
void Daemon::lost_position(const char *why) {
  wmv_log(ERROR_LEVEL,
          "the saved binary log position (%s, %llu) cannot be used: %s. All "
          "SYNCHRONOUS materialized views are refreshed and the daemon "
          "continues from the end of the binary log.",
          saved_file.c_str(), static_cast<unsigned long long>(saved_pos), why);
  have_saved_position = false;
  refresh_all_pending = true;
  registry_loaded = false; /* decide again which views are refreshed */
  close_reader();
  discard_transaction();
  xa_prepared.clear();
  batch.clear();
}

/**
  Position the reader: at the saved position, or at the end of the binary
  log when there is none.
*/
bool Daemon::init_position() {
  std::string why;
  if (have_saved_position) {
    if (open_reader(saved_file.c_str(), saved_pos, &why)) {
      lost_position(why.c_str());
      return true; /* comes back without a saved position */
    }
  } else {
    Log_info current;
    if (mysql_bin_log.get_current_log(&current, true /* LOCK_log */) ||
        current.log_file_name[0] == 0) {
      wmv_log(ERROR_LEVEL, "cannot find the current binary log file");
      return true;
    }
    if (open_reader(current.log_file_name, current.pos, &why)) {
      wmv_log(ERROR_LEVEL, "cannot read the binary log: %s", why.c_str());
      return true;
    }
    saved_file = linfo.log_file_name;
    saved_pos = current.pos;
    have_saved_position = true;
    mview_set_applied_position(saved_file, saved_pos);
    wmv_log(INFORMATION_LEVEL, "starting at the end of the binary log (%s, %llu)",
            saved_file.c_str(), static_cast<unsigned long long>(saved_pos));
    save_state();
  }
  /* What is before the position is committed in the engines before any
     view is looked at. */
  while (commit_barrier()) {
    if (should_stop()) return true;
    absorb_kill();
  }
  in_trx = false;
  trx.clear();
  table_maps.clear();
  positioned = true;
  read_failures = 0;
  return false;
}

void Daemon::wait_for_binlog(my_off_t pos, unsigned long milliseconds) {
  PSI_stage_info old_stage;
  mysql_bin_log.lock_binlog_end_pos();
  thd->ENTER_COND(mysql_bin_log.get_log_cond(),
                  mysql_bin_log.get_binlog_end_pos_lock(), &stage_wmv_waiting,
                  &old_stage);
  if (mysql_bin_log.get_binlog_end_pos() <= pos &&
      mysql_bin_log.is_active(linfo.log_file_name) && !thd->killed &&
      !stop_requested.load())
    mysql_bin_log.wait_for_update(std::chrono::milliseconds(milliseconds));
  mysql_bin_log.unlock_binlog_end_pos();
  thd->EXIT_COND(&old_stage);
}

/* The file is read to its end: continue with the next one of the index. */
bool Daemon::next_file(std::string *why) {
  if (in_trx) {
    /* a transaction that the server did not finish before it stopped */
    discard_transaction();
  }
  table_maps.clear();
  mysql_bin_log.lock_index();
  const int error = mysql_bin_log.find_next_log(&linfo, false);
  mysql_bin_log.unlock_index();
  if (error) {
    *why = "there is no binary log file after the one that was read";
    return true;
  }
  reader.reset(new Binlog_file_reader(true));
  if (reader->open(linfo.log_file_name, BIN_LOG_HEADER_SIZE)) {
    *why = std::string("cannot open ") + linfo.log_file_name + ": " +
           reader->get_error_str();
    return true;
  }
  {
    std::lock_guard<std::mutex> guard(status_mutex);
    st_read_file = linfo.log_file_name;
  }
  st_read_pos.store(reader->position());
  if (!in_trx) {
    batch.end_file = linfo.log_file_name;
    batch.end_pos = reader->position();
    batch.has_end = true;
  }
  return false;
}

void Daemon::discard_transaction() {
  in_trx = false;
  trx.clear();
  xa_xid.clear();
  table_maps.clear();
}

/* A complete transaction: its rows go to the batch, with the position
   where it ends (the reader is right behind its last event). */
void Daemon::add_transaction(Changes *changes, long long when) {
  const unsigned number = static_cast<unsigned>(batch.trx_ends.size());
  batch.trx_ends.emplace_back(linfo.log_file_name,
                              reader ? reader->position() : 0);
  batch.changes.merge(changes, number);
  batch.transactions++;
  if (when > 0) {
    batch.last_commit_ts = when;
    st_last_read_ts.store(when);
  }
}

void Daemon::commit_transaction(long long when) {
  add_transaction(&trx, when);
  in_trx = false;
  xa_xid.clear();
  table_maps.clear();
}

void Daemon::process_table_map(Table_map_log_event *ev) {
  const unsigned long long id = ev->get_table_id().id();
  const std::string key = table_key(ev->get_db_name(), ev->get_table_name());
  if (tracked.find(key) == tracked.end()) {
    table_maps.erase(id); /* the id may have been another table */
    return;
  }
  std::unique_ptr<Table_map_info> info(new Table_map_info);
  if (info->init(ev)) {
    /* kept without a table_def: its rows cannot be decoded */
    delete info->def;
    info->def = nullptr;
    info->db = ev->get_db_name();
    info->table = ev->get_table_name();
  }
  table_maps[id] = std::move(info);
}

void Daemon::process_rows(Rows_log_event *ev) {
  const unsigned long long id = ev->get_table_id().id();
  auto map = table_maps.find(id);
  if (map != table_maps.end()) {
    const std::string key = table_key(map->second->db, map->second->table);
    auto table = tracked.find(key);
    if (table != tracked.end() && !trx.refresh_tables.count(key)) {
      Table_changes &changes = trx.tables[key];
      changes.columns = table->second.columns;
      std::vector<Row_change> &rows = changes.rows;
      const size_t before = rows.size();
      std::string why;
      if (decode_rows(*map->second, ev, table->second.columns, &rows, &why)) {
        rows.resize(before);
        trx.rows -= before;
        trx.tables.erase(key);
        trx.refresh_tables.insert(key);
        if (logged_undecodable.insert(key).second)
          wmv_log(WARNING_LEVEL,
                  "the row events of %s cannot be decoded (%s); its "
                  "materialized views are refreshed completely",
                  printable_key(key).c_str(), why.c_str());
      } else {
        trx.rows += rows.size() - before;
      }
    }
  }
  if (ev->get_flags(Rows_log_event::STMT_END_F)) table_maps.clear();
}

/**
  Statements in the binary log: the transaction boundaries, and statements
  that change tables without row events (DDL; DML of a session that logs in
  statement format).  The views of a tracked table that such a statement
  names are refreshed completely.
*/
void Daemon::process_query(Query_log_event *ev) {
  const char *query = ev->query;
  const size_t length = ev->q_len;
  if (query == nullptr || length == 0) return;
  const long long when = static_cast<long long>(ev->common_header->when.tv_sec);

  std::vector<std::string> words;
  size_t rest = length;
  leading_words(query, length, 12, &words, &rest);
  if (words.empty()) return;
  const std::string &w0 = words[0];
  const std::string w1 = words.size() > 1 ? words[1] : std::string();

  if (w0 == "BEGIN" || (w0 == "START" && w1 == "TRANSACTION")) {
    discard_transaction();
    in_trx = true;
    return;
  }
  if (w0 == "COMMIT") {
    commit_transaction(when);
    return;
  }
  if (w0 == "ROLLBACK") {
    if (w1 != "TO") discard_transaction();
    return;
  }
  if (w0 == "SAVEPOINT" || w0 == "RELEASE") return;
  if (w0 == "XA") {
    std::string xid = trimmed(query + rest, length - rest);
    if (w1 == "START" || w1 == "BEGIN") {
      discard_transaction();
      in_trx = true;
      xa_xid = xid;
    } else if (w1 == "COMMIT" || w1 == "ROLLBACK") {
      /* XA COMMIT xid [ONE PHASE] */
      if (xid.size() > 9 &&
          strcasecmp(xid.c_str() + xid.size() - 9, "ONE PHASE") == 0)
        xid = trimmed(xid.c_str(), xid.size() - 9);
      auto prepared = xa_prepared.find(xid);
      if (in_trx && xa_xid == xid) {
        if (w1 == "COMMIT")
          commit_transaction(when);
        else
          discard_transaction();
      } else if (prepared != xa_prepared.end()) {
        if (w1 == "COMMIT") add_transaction(&prepared->second, when);
        xa_prepared.erase(prepared);
      } else if (w1 == "COMMIT") {
        /* prepared before the daemon started reading: its rows are not
           known */
        if (!tracked.empty()) {
          wmv_log(WARNING_LEVEL,
                  "XA COMMIT of a transaction whose rows were not read; all "
                  "materialized views are refreshed");
          batch.changes.refresh_all = true;
          batch.close = true;
        }
      }
    }
    return;
  }

  /* statements that do not change the rows or the layout of a table */
  if (w0 == "ANALYZE" || w0 == "GRANT" || w0 == "REVOKE" || w0 == "FLUSH" ||
      w0 == "SET" || w0 == "REFRESH" || w0 == "OPTIMIZE" || w0 == "REPAIR" ||
      w0 == "USE")
    return;
  bool drop_schema = false;
  if (w0 == "CREATE" || w0 == "ALTER" || w0 == "DROP") {
    if (w1 == "TEMPORARY") return;
    for (size_t i = 1; i < words.size(); i++) {
      const std::string &w = words[i];
      if (w == "TABLE" || w == "TABLES" || w == "INDEX") break;
      if (w == "DATABASE" || w == "SCHEMA") {
        drop_schema = w0 == "DROP";
        break;
      }
      if (w == "VIEW" || w == "MATERIALIZED" || w == "TRIGGER" ||
          w == "PROCEDURE" || w == "FUNCTION" || w == "EVENT" || w == "USER" ||
          w == "ROLE" || w == "SERVER" || w == "TABLESPACE" ||
          w == "LOGFILE" || w == "RESOURCE" || w == "INSTANCE" || w == "UNDO")
        return;
    }
  }

  bool any = false;
  Changes &target = in_trx ? trx : batch.changes;
  /* the schema of the session that ran the statement: a table that the
     statement names without a schema is of it */
  const std::string default_db =
      ev->db != nullptr ? std::string(ev->db, ev->db_len) : std::string();
  for (const auto &entry : tracked) {
    const Tracked_table &table = entry.second;
    if (mentions_table(query, length, table.db, table.table, default_db) ||
        (drop_schema && mentions(query, length, table.db))) {
      target.refresh_tables.insert(entry.first);
      any = true;
    }
  }
  if (any && !in_trx) {
    /* the definitions may have changed: load them before going on */
    batch.reload = true;
    batch.close = true;
    wmv_log(INFORMATION_LEVEL,
            "a statement on a base table of a materialized view was read "
            "from the binary log; its views are refreshed: %.200s",
            std::string(query, length).c_str());
  }
}

void Daemon::process_event(Log_event *ev) {
  switch (ev->get_type_code()) {
    case TABLE_MAP_EVENT:
      process_table_map(static_cast<Table_map_log_event *>(ev));
      break;

    case WRITE_ROWS_EVENT:
    case UPDATE_ROWS_EVENT:
    case DELETE_ROWS_EVENT:
    case PARTIAL_UPDATE_ROWS_EVENT:
      process_rows(static_cast<Rows_log_event *>(ev));
      break;

    case XID_EVENT:
      commit_transaction(
          static_cast<long long>(ev->common_header->when.tv_sec));
      break;

    case QUERY_EVENT:
      process_query(static_cast<Query_log_event *>(ev));
      break;

    case XA_PREPARE_LOG_EVENT: {
      XA_prepare_log_event *xa = static_cast<XA_prepare_log_event *>(ev);
      if (xa->is_one_phase()) {
        commit_transaction(
            static_cast<long long>(ev->common_header->when.tv_sec));
      } else {
        /* applied when XA COMMIT is read */
        if (!trx.empty()) xa_prepared[xa_xid].merge(&trx, 0);
        discard_transaction();
      }
      break;
    }

    case TRANSACTION_PAYLOAD_EVENT: {
      /* a compressed transaction: the events are inside */
      Transaction_payload_log_event *payload =
          static_cast<Transaction_payload_log_event *>(ev);
      binlog::Decompressing_event_object_istream stream(
          *payload, reader->format_description_event());
      std::shared_ptr<Log_event> inner;
      while (stream >> inner) process_event(inner.get());
      if (stream.has_error()) {
        wmv_log(ERROR_LEVEL,
                "cannot decompress a transaction of the binary log (%s); all "
                "materialized views are refreshed",
                stream.get_error_str().c_str());
        discard_transaction();
        batch.changes.refresh_all = true;
        batch.close = true;
      }
      break;
    }

    case INCIDENT_EVENT:
      /* the binary log may lack changes */
      if (!tracked.empty()) {
        wmv_log(WARNING_LEVEL,
                "incident event in the binary log; all materialized views "
                "are refreshed");
        batch.changes.refresh_all = true;
        batch.close = true;
      }
      break;

    default:
      /* format description, rotate, stop, GTIDs, rows query, ... */
      break;
  }
}

/**
  Read complete transactions into the batch.

  BATCH   the batch is to be applied now
  IDLE    nothing to apply (the daemon waited for the binary log)
  FAILED  the binary log cannot be read at this position
*/
Daemon::Read_result Daemon::read_batch(std::string *why) {
  for (;;) {
    if (should_stop()) return Read_result::STOP;
    const my_off_t pos = reader->position();
    const bool in_target_file =
        target_set &&
        compare_log_name(linfo.log_file_name, target_file.c_str()) == 0;
    if (target_set) {
      /* exactly up to the position noted while the tables were locked */
      if (in_target_file && pos >= target_pos) return Read_result::BATCH;
    } else {
      if (!sv_enabled)
        return batch.has_work() ? Read_result::BATCH : Read_result::IDLE;
      if (mview_registry_version.load() != registry_version && !in_trx)
        /* new views: their tables must be tracked from here on */
        return batch.has_work() ? Read_result::BATCH : Read_result::IDLE;
    }

    if (limit_set && reading_active && pos >= limit) limit_set = false;
    if (!limit_set) {
      /* As the dump thread: the end position first, then whether the file
         is the active one. */
      const my_off_t end = mysql_bin_log.get_binlog_end_pos();
      const bool active = mysql_bin_log.is_active(linfo.log_file_name);
      if (active && pos >= end) {
        if (target_set) {
          /* written and not yet readable: a moment */
          wait_for_binlog(pos, 10);
          continue;
        }
        if (batch.has_work()) return Read_result::BATCH;
        st_caught_up.store(true);
        set_state("waiting for the binary log");
        wait_for_binlog(pos, sv_idle_wait_timeout);
        return Read_result::IDLE;
      }
      st_caught_up.store(false);
      set_state("reading the binary log");
      if (commit_barrier()) {
        absorb_kill();
        continue;
      }
      limit = active ? end : 0;
      if (in_target_file && (limit == 0 || limit > target_pos))
        limit = target_pos;
      reading_active = active || in_target_file;
      limit_set = true;
    }

    Log_event *ev = reader->read_event_object();
    if (ev == nullptr) {
      if (reader->has_fatal_error()) {
        *why = std::string("error reading ") + linfo.log_file_name + " at " +
               std::to_string(pos) + ": " + reader->get_error_str();
        return Read_result::FAILED;
      }
      if (reading_active) {
        *why = std::string("unexpected end of ") + linfo.log_file_name +
               " at " + std::to_string(pos);
        return Read_result::FAILED;
      }
      if (in_target_file) {
        *why = "the binary log ends before the position that was noted";
        return Read_result::FAILED;
      }
      if (next_file(why)) return Read_result::FAILED;
      limit_set = false;
      continue;
    }
    if (!in_trx && mview_registry_version.load() != registry_version) {
      /* The views changed.  This event may be the first of a transaction
         that changes the tables of a new view: it is read again when the
         views are loaded (see the adoption of views in load_registry). */
      delete ev;
      limit_set = false;
      if (reader->seek(pos)) {
        *why = "cannot go back in the binary log";
        return Read_result::FAILED;
      }
      return batch.has_work() || target_set ? Read_result::BATCH
                                            : Read_result::IDLE;
    }
    process_event(ev);
    delete ev;
    read_failures = 0;

    st_read_pos.store(reader->position());
    if (!in_trx) {
      batch.end_file = linfo.log_file_name;
      batch.end_pos = reader->position();
      batch.has_end = true;
      if (!target_set &&
          (batch.close || batch.changes.rows >= sv_batch_rows ||
           batch.transactions >= sv_batch_transactions))
        return Read_result::BATCH;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Applier                                                             */
/* ------------------------------------------------------------------ */


/* The row keys of the changed rows of every table of the batch */
/* A join view with a WHERE has changes of instances other than its root
   that are not applied: it must be applied with its base tables locked. */
bool Daemon::join_where_needs_pin(size_t view) const {
  const Mview_def &def = views[view];
  const View_rt &v = rt[view];
  if (def.is_aggregate || !v.join_where || !v.maintained || v.needs_refresh)
    return false;
  const unsigned start = join_where_start(view);
  for (size_t t = 0; t < def.tables.size(); t++) {
    if (t == def.root) continue;
    auto changes = batch.changes.tables.find(
        table_key(def.tables[t].db, def.tables[t].table));
    if (changes == batch.changes.tables.end()) continue;
    for (const Row_change &row : changes->second.rows)
      if (row.trx >= start) return true;
  }
  return false;
}

void Daemon::make_key_sets(std::map<std::string, Key_sets> *keys,
                           unsigned from_trx) {
  /* the row keys of the instances of the join views (key_set_id()) */
  std::map<std::string, const Mview_table *> instances;
  for (size_t i = 0; i < views.size(); i++) {
    if (views[i].is_aggregate || !(rt[i].maintained || rt[i].needs_refresh))
      continue;
    for (const Mview_table &table : views[i].tables)
      instances.emplace(key_set_id(table), &table);
  }
  for (const auto &instance : instances) {
    const Mview_table &table = *instance.second;
    const std::string tkey = table_key(table.db, table.table);
    auto changes = batch.changes.tables.find(tkey);
    if (changes == batch.changes.tables.end()) continue;
    const auto &entry = *changes;
    Key_sets &sets = (*keys)[instance.first];
    if (tracked.find(tkey) == tracked.end()) {
      sets.bad = true;
      sets.why = "the table is not tracked any more";
      continue;
    }
    std::vector<unsigned> slots;
    if (table.pk_ordinals.empty()) {
      sets.bad = true;
      sets.why = "it has no columns that can identify its rows";
    }
    for (unsigned ordinal : table.pk_ordinals) {
      const int slot = entry.second.slot(ordinal);
      if (slot < 0) {
        sets.bad = true;
        sets.why = "the row key was not decoded";
        break;
      }
      slots.push_back(static_cast<unsigned>(slot));
    }
    const bool single = slots.size() == 1;
    std::string old_key, new_key;

    for (const Row_change &row : entry.second.rows) {
      if (sets.bad) break;
      if (row.trx < from_trx) continue;
      old_key.clear();
      new_key.clear();
      bool old_null = false, new_null = false;
      const bool has_old = row.kind != Row_change::INSERT;
      const bool has_new = row.kind != Row_change::DELETE;
      if (!single) {
        old_key.push_back('(');
        new_key.push_back('(');
      }
      for (size_t k = 0; k < slots.size() && !sets.bad; k++) {
        const Cell *before =
            has_old && slots[k] < row.before.size() ? &row.before[slots[k]]
                                                    : nullptr;
        const Cell *after =
            has_new && slots[k] < row.after.size() ? &row.after[slots[k]]
                                                   : nullptr;
        /* binlog_row_image=MINIMAL: the after image of an update has a
           key column only when it changed */
        if (has_old && after != nullptr && after->state == Cell_state::ABSENT)
          after = before;
        for (int image = 0; image < 2; image++) {
          const bool wanted = image == 0 ? has_old : has_new;
          if (!wanted) continue;
          const Cell *cell = image == 0 ? before : after;
          std::string &key = image == 0 ? old_key : new_key;
          const bool null_value = cell != nullptr &&
                                  cell->state == Cell_state::SQL_NULL &&
                                  table.key_nullable;
          if (!null_value &&
              (cell == nullptr || cell->state != Cell_state::VALUE ||
               !cell->exact)) {
            sets.bad = true;
            if (cell == nullptr || cell->state == Cell_state::ABSENT)
              sets.why = "a row key column is not in the row image";
            else if (cell->state == Cell_state::SQL_NULL)
              sets.why = "a primary key column is NULL";
            else if (cell->state == Cell_state::UNSUPPORTED)
              sets.why = cell->literal;
            else
              sets.why = "a row key column has no exact literal (FLOAT)";
            break;
          }
          if (null_value) (image == 0 ? old_null : new_null) = true;
          if (k) key.push_back(',');
          key.append(null_value ? std::string("NULL") : cell->literal);
        }
      }
      if (sets.bad) break;
      if (!single) {
        old_key.push_back(')');
        new_key.push_back(')');
      }
      if (has_old) (old_null ? sets.old_null_keys : sets.old_keys).insert(old_key);
      if (has_new) (new_null ? sets.new_null_keys : sets.new_keys).insert(new_key);
    }
    if (sets.bad) {
      sets.old_keys.clear();
      sets.new_keys.clear();
      sets.old_null_keys.clear();
      sets.new_null_keys.clear();
      if (logged_undecodable.insert(instance.first).second)
        wmv_log(WARNING_LEVEL,
                "the row keys of the changed rows of %s cannot be used "
                "(%s); its materialized views are refreshed completely",
                printable_key(tkey).c_str(), sets.why.c_str());
    }
  }
}

/* Make the temporary table K of a view: one row per root key that is
   recomputed, with the column types of the row key of the root. */
bool Daemon::ensure_key_table(size_t view, Sql_error *error) {
  View_rt &v = rt[view];
  if (v.tmp_ready) return false;
  const Mview_def &def = views[view];
  const Mview_table &root = def.tables[def.root];

  for (const std::string &table : v.tmp_tables)
    exec("DROP TEMPORARY TABLE IF EXISTS " + table);
  std::string select = " ENGINE=InnoDB AS SELECT ";
  std::string cols;
  for (size_t k = 0; k < root.pk_columns.size(); k++) {
    if (k) {
      select += ", ";
      cols += ", ";
    }
    select += qi(root.alias) + "." + qi(root.pk_columns[k]) + " AS " +
              v.key_cols[k];
    cols += v.key_cols[k];
  }
  select += " FROM " + qname(root.db, root.table) + " AS " + qi(root.alias) +
            " LIMIT 0";
  /* The primary key only removes duplicates; a key that cannot be
     indexed (a prefix of a TEXT column) works without it.  A nullable root
     key gets a UNIQUE key: a PRIMARY KEY would make its columns NOT NULL
     and INSERT IGNORE would turn NULLs into zeros. */
  if (exec("CREATE TEMPORARY TABLE " + v.key_table +
               (root.key_nullable ? " (UNIQUE KEY (" : " (PRIMARY KEY (") +
               cols + "))" + select,
           error)) {
    exec("ROLLBACK");
    if (is_transient(error->code)) return true;
    if (exec("CREATE TEMPORARY TABLE " + v.key_table + select, error)) {
      exec("ROLLBACK");
      return true;
    }
  }
  if (v.join_where) {
    for (size_t t = 0; t < def.tables.size(); t++) {
      if (create_delta_table(def.tables[t], v.tmp_tables[1 + t], error)) {
        exec("ROLLBACK");
        return true;
      }
    }
  }
  if (exec("COMMIT", error)) return true;
  v.tmp_ready = true;
  return false;
}

/**
  Run head + <keys> + tail for all keys, sv_in_list_chunk_size at a time.

  @param as_values      the keys are rows of INSERT ... VALUES
  @param single_column  the key is one column (not parenthesized)
*/
bool Daemon::insert_keys(const std::string &head, const std::string &tail,
                         const std::unordered_set<std::string> &keys,
                         bool as_values, bool single_column,
                         Sql_error *error) {
  const size_t chunk = sv_in_list_chunk_size ? sv_in_list_chunk_size : 1;
  std::string sql;
  size_t n = 0;
  for (const std::string &key : keys) {
    if (n == 0) {
      sql = head;
    } else {
      sql.push_back(',');
    }
    if (as_values && single_column) {
      sql.push_back('(');
      sql.append(key);
      sql.push_back(')');
    } else {
      sql.append(key);
    }
    if (++n == chunk) {
      sql.append(tail);
      if (exec(sql, error)) return true;
      n = 0;
    }
  }
  if (n > 0) {
    sql.append(tail);
    if (exec(sql, error)) return true;
  }
  return false;
}

/**
  Run head + "lhs <=> key OR ..." + ")" for keys with a NULL,
  sv_in_list_chunk_size at a time.
*/
bool Daemon::insert_null_keys(const std::string &head, const std::string &lhs,
                              const std::unordered_set<std::string> &keys,
                              Sql_error *error) {
  const size_t chunk = sv_in_list_chunk_size ? sv_in_list_chunk_size : 1;
  std::string sql;
  size_t n = 0;
  for (const std::string &key : keys) {
    sql += (n == 0 ? head : std::string(" OR ")) + lhs + " <=> " + key;
    if (++n == chunk) {
      if (exec(sql + ")", error)) return true;
      sql.clear();
      n = 0;
    }
  }
  if (n > 0 && exec(sql + ")", error)) return true;
  return false;
}

/**
  One attempt to apply the batch to a join view, in one transaction:

    K := root keys of the changed rows
    delete the rows of the view whose root key is in K
    insert the rows of the join whose root key is in K
    [record the position of the view - not needed while this is idempotent]
    commit

  @retval false committed
  @retval true  error; the caller rolls back
*/
bool Daemon::apply_view_once(size_t view,
                             const std::map<std::string, Key_sets> &keys,
                             Sql_error *error, std::string *need_refresh) {
  const Mview_def &def = views[view];
  View_rt &v = rt[view];
  const Mview_table &root = def.tables[def.root];
  const bool root_single = root.pk_columns.size() == 1;

  mview_set_maintenance_mode(thd, true);
  /* a multi-table DELETE resolves its target alias in the current schema */
  if (exec("USE " + qi(def.db), error)) return true;
  if (ensure_key_table(view, error)) return true;

  std::string k_cols;
  for (size_t k = 0; k < v.key_cols.size(); k++) {
    if (k) k_cols += ", ";
    k_cols += v.key_cols[k];
  }
  const std::string insert_k =
      "INSERT IGNORE INTO " + v.key_table + " (" + k_cols + ") ";

  /* -- begin (autocommit is off: the first statement starts it) -- */
  if (exec("DELETE FROM " + v.key_table, error)) return true;

  /* -- 1. root keys -- */
  std::string root_pk_list; /* `r`.`pk0`, `r`.`pk1` */
  std::string detail_root_list; /* `warp_mvd`.`key0`, ... */
  for (size_t k = 0; k < root.pk_columns.size(); k++) {
    if (k) {
      root_pk_list += ", ";
      detail_root_list += ", ";
    }
    root_pk_list += qi(root.alias) + "." + qi(root.pk_columns[k]);
    detail_root_list += "`warp_mvd`." + qi(root.key_columns[k]);
  }

  for (size_t t = 0; t < def.tables.size(); t++) {
    const Mview_table &table = def.tables[t];
    auto sets = keys.find(key_set_id(table));
    if (sets == keys.end()) continue;
    const bool single = table.pk_columns.size() == 1;

    if (t == def.root) {
      /* the old and the new keys of the root are root keys */
      const std::string head = insert_k + "VALUES ";
      if (insert_keys(head, "", sets->second.old_keys, true, single, error) ||
          insert_keys(head, "", sets->second.new_keys, true, single, error) ||
          insert_keys(head, "", sets->second.old_null_keys, true, single,
                      error) ||
          insert_keys(head, "", sets->second.new_null_keys, true, single,
                      error))
        return true;
      continue;
    }

    std::string detail_t_list, alias_pk_list;
    for (size_t k = 0; k < table.pk_columns.size(); k++) {
      if (k) {
        detail_t_list += ", ";
        alias_pk_list += ", ";
      }
      detail_t_list += "`warp_mvd`." + qi(table.key_columns[k]);
      alias_pk_list += qi(table.alias) + "." + qi(table.pk_columns[k]);
    }
    if (!single) {
      detail_t_list = "(" + detail_t_list + ")";
      alias_pk_list = "(" + alias_pk_list + ")";
    }
    /* old keys: the rows of the view that were made from the row */
    const std::string old_head = insert_k + "SELECT DISTINCT " +
                                 detail_root_list + " FROM " + v.name +
                                 " AS `warp_mvd` WHERE ";
    if (insert_keys(old_head + detail_t_list + " IN (", ")",
                    sets->second.old_keys, false, single, error) ||
        insert_null_keys(old_head + "(", detail_t_list,
                         sets->second.old_null_keys, error))
      return true;
    /* new keys: the root rows the row joins with now.  The condition on
       the table makes the outer joins towards it inner joins.  The WHERE
       of the view is not applied: a root row whose only match fails it
       loses its NULL-extended row. */
    const std::string new_head = insert_k + "SELECT DISTINCT " +
                                 root_pk_list + " " + def.from_sql + " WHERE ";
    if (insert_keys(new_head + alias_pk_list + " IN (", ")",
                    sets->second.new_keys, false, single, error) ||
        insert_null_keys(new_head + "(", alias_pk_list,
                         sets->second.new_null_keys, error))
      return true;
  }

  if (v.join_where) {
    /* Old rows of the join that the WHERE filtered out are not in the
       view, but when they go away a NULL-extended row of their root row
       may appear.  They are found through the join itself: every changed
       instance in turn is its old images and must be matched; every other
       changed instance is tried as its base table (rows that did not
       change) and as its old images (rows that changed in the same
       batch); the root and the unchanged instances are their base tables.
       An old row of the join consists of old images of changed rows and
       of rows that are still there, so one of the combinations has it. */
    std::vector<size_t> changed;
    for (size_t t = 0; t < def.tables.size(); t++) {
      if (t == def.root) continue;
      if (exec("DELETE FROM " + v.tmp_tables[1 + t], error)) return true;
      size_t rows = 0;
      if (fill_delta(view, t, v.tmp_tables[1 + t], true, -1, 0,
                     join_from_trx, &rows, need_refresh, error))
        return true;
      if (!need_refresh->empty()) return false;
      if (rows > 0) changed.push_back(t);
    }
    if (changed.size() > 4) {
      *need_refresh = "too many tables changed at once";
      return false;
    }
    for (size_t target : changed) {
      const Mview_table &table = def.tables[target];
      /* a row of the delta table, not a NULL-extended row: the sign
         column is never NULL, a column of the row key may be */
      const std::string matched =
          qi(table.alias) + ".`mv$sign` IS NOT NULL";
      std::vector<size_t> others;
      for (size_t t : changed)
        if (t != target) others.push_back(t);
      for (unsigned mask = 0; mask < (1U << others.size()); mask++) {
        std::vector<std::string> sources;
        for (const Mview_table &source : def.tables)
          sources.push_back(qname(source.db, source.table));
        sources[target] = v.tmp_tables[1 + target];
        for (size_t o = 0; o < others.size(); o++)
          if (mask & (1U << o))
            sources[others[o]] = v.tmp_tables[1 + others[o]];
        std::string from = def.from_template;
        for (size_t t = 0; t < sources.size(); t++) {
          const std::string token = "`mv$source$" + std::to_string(t) + "`";
          size_t at = 0;
          while ((at = from.find(token, at)) != std::string::npos) {
            from.replace(at, token.size(), sources[t]);
            at += sources[t].size();
          }
        }
        if (exec(insert_k + "SELECT DISTINCT " + root_pk_list + " " + from +
                     " WHERE " + matched,
                 error))
          return true;
      }
    }
  }

  long long root_keys = 0;
  if (exec("SELECT CAST(COUNT(*) AS CHAR) FROM " + v.key_table, error, &root_keys))
    return true;

  if (root_keys > 0) {
    /* -- 2. delete the rows of the root keys -- */
    std::string on;
    for (size_t k = 0; k < root.pk_columns.size(); k++) {
      if (k) on += " AND ";
      on += "`warp_mvd`." + qi(root.key_columns[k]) +
            (root.key_nullable ? " <=> `warp_mvk`." : " = `warp_mvk`.") +
            v.key_cols[k];
    }
    if (exec("DELETE `warp_mvd` FROM " + v.name + " AS `warp_mvd` JOIN " +
                 v.key_table + " AS `warp_mvk` ON " + on,
             error))
      return true;

    /* -- 3. insert them again from the join -- */
    std::string k_select;
    for (size_t k = 0; k < v.key_cols.size(); k++) {
      if (k) k_select += ", ";
      k_select += "`warp_mvk`." + v.key_cols[k];
    }
    std::string sql = "INSERT INTO " + v.name + " (" + def.detail_columns +
                      ") " + def.detail_select + " " + def.from_sql + " WHERE ";
    if (!def.where_sql.empty()) sql += "(" + def.where_sql + ") AND ";
    if (root.key_nullable) {
      std::string match;
      for (size_t k = 0; k < root.pk_columns.size(); k++)
        match += std::string(k ? " AND " : "") + qi(root.alias) + "." +
                 qi(root.pk_columns[k]) + " <=> `warp_mvk`." + v.key_cols[k];
      sql += "EXISTS (SELECT 1 FROM " + v.key_table + " AS `warp_mvk` WHERE " +
             match + ")";
    } else {
      sql += (root_single ? root_pk_list : "(" + root_pk_list + ")") +
             " IN (SELECT " + k_select + " FROM " + v.key_table +
             " AS `warp_mvk`)";
    }
    if (exec(sql, error)) return true;
  }

  /* -- [record position] --
     Not needed: recomputing by root key may be repeated.  (Aggregate views
     record theirs here, see apply_agg_once().) */

  /* -- commit -- */
  if (exec("COMMIT", error)) return true;
  st_keys.fetch_add(static_cast<unsigned long long>(root_keys));
  return false;
}

/**
  After a failed attempt: roll back, decide what is next.

  OK       try again
  STOP     the daemon stops
  RESTART  the registry changed; the batch is applied again after a reload
  and *give_up when the error is not transient and the attempts are used up.
*/
Apply_result Daemon::after_error(const Sql_error &error, unsigned *attempt,
                                 bool *give_up, const char *what,
                                 const std::string &view) {
  *give_up = false;
  st_apply_errors.fetch_add(1);
  const bool stopping = should_stop();
  rollback();
  if (stopping || should_stop()) return Apply_result::STOP;
  absorb_kill();

  const bool transient = is_transient(error.code);
  (*attempt)++;
  if (*attempt == 1 || !transient)
    log_limited(WARNING_LEVEL,
                view + what + std::to_string(error.code),
                std::string(what) + " of materialized view " + view +
                    " failed: " + std::to_string(error.code) + " " +
                    error.message + "; statement: " + last_failed_sql);
  if (mview_registry_version.load() != registry_version && !transient)
    return Apply_result::RESTART; /* e.g. the view was dropped */
  if (!transient && *attempt >= sv_retry_limit) {
    *give_up = true;
    return Apply_result::OK;
  }
  /* locks are expected now and then: wait a little longer every time */
  pause(transient ? std::min<unsigned long>(100UL * *attempt, 5000UL) : 100UL);
  if (should_stop()) return Apply_result::STOP;
  if (!sv_enabled && transient) {
    /* paused while waiting for a lock: the batch is kept */
    return Apply_result::RESTART;
  }
  return Apply_result::OK;
}

Apply_result Daemon::refresh_view(size_t view, const char *reason) {
  View_rt &v = rt[view];
  wmv_log(INFORMATION_LEVEL, "refreshing materialized view %s: %s",
          v.name.c_str(), reason);
  /* REFRESH locks the base tables itself and may wait for writers that
     are queued behind the read locks of this daemon: never refresh while
     holding them.  The rest of the batch is applied again under a new
     pin; this view skips what the refresh already covers. */
  const bool was_pinned = pin_held;
  if (was_pinned) release_pin();
  unsigned attempt = 0;
  for (;;) {
    Sql_error error;
    mview_set_maintenance_mode(thd, true);
    if (!exec("REFRESH MATERIALIZED VIEW " + v.name, &error) &&
        !exec("COMMIT", &error)) {
      mview_set_maintenance_mode(thd, true);
      st_full_refreshes.fetch_add(1);
      v.tmp_ready = false;
      v.force_pin = false;
      if (views[view].is_aggregate) {
        /* REFRESH records the position the view is exact as of */
        load_index_files();
        const bool failed = read_view_position(view, false, &error);
        exec(failed ? "ROLLBACK" : "COMMIT");
        if (failed || !position_usable(view)) {
          mark_stale(view, "REFRESH MATERIALIZED VIEW did not record a "
                           "binary log position that is in the binary log");
          return Apply_result::FAILED;
        }
        trx_ordinals.clear();
        for (const auto &end : batch.trx_ends)
          trx_ordinals.push_back(file_ordinal(end.first));
      }
      mark_in_sync(view);
      refreshed[v.name] = depends_mark();
      return was_pinned ? Apply_result::REPIN : Apply_result::OK;
    }
    bool give_up = false;
    const Apply_result next =
        after_error(error, &attempt, &give_up, "REFRESH", v.name);
    mview_set_maintenance_mode(thd, true);
    if (next != Apply_result::OK) return next;
    if (give_up) {
      mark_stale(view, ("REFRESH MATERIALIZED VIEW failed: " +
                        std::to_string(error.code) + " " + error.message)
                           .c_str());
      return Apply_result::FAILED;
    }
  }
}

Apply_result Daemon::apply_view(size_t view,
                                const std::map<std::string, Key_sets> &keys) {
  View_rt &v = rt[view];
  unsigned attempt = 0;
  for (;;) {
    Sql_error error;
    std::string need_refresh;
    if (!apply_view_once(view, keys, &error, &need_refresh)) {
      if (need_refresh.empty()) return Apply_result::OK;
      rollback();
      if (should_stop()) return Apply_result::STOP;
      log_limited(WARNING_LEVEL, v.name + ": " + need_refresh,
                  "materialized view " + v.name +
                      " has a WHERE and cannot be updated from the binary "
                      "log (" + need_refresh +
                      "); it is refreshed completely instead");
      return refresh_view(view, need_refresh.c_str());
    }
    v.tmp_ready = false; /* made again by the next attempt */
    bool give_up = false;
    const Apply_result next =
        after_error(error, &attempt, &give_up, "the update", v.name);
    if (next != Apply_result::OK) return next;
    if (give_up)
      return refresh_view(view, "its changes could not be applied");
  }
}

/* ------------------------------------------------------------------ */
/* Aggregate views                                                     */
/* ------------------------------------------------------------------ */

/*
  The table of an aggregate view has one row per group: the group columns,
  the row count of the group and the aggregate states (sql/mview.h).  The
  old and new images of the changed base rows are loaded into one temporary
  "delta" table per changed table instance (the referenced columns plus
  `mv$sign`: -1 for an old image, +1 for a new one), and the FROM clause of
  the view is evaluated with delta tables in the place of base tables.

  A view that reads its base tables for this (every view but a
  delta-capable view of one table) must see them exactly as of the end of
  the batch.  The daemon "pins" them: its second session takes LOCK TABLES
  ... READ on them, which waits for the transactions that wrote them; the
  end of the binary log is noted and read up to; the views are updated and
  the tables are unlocked.  The lock is kept while the views are updated
  for all engines: the statements that read the base tables are INSERT ...
  SELECT, which InnoDB does not read from the snapshot of a REPEATABLE
  READ transaction (it uses locking reads, see ha_innobase::store_lock()),
  so a snapshot taken under the lock would not be used.

  Adding deltas is not idempotent.  The state table of the view has the
  binary log position up to which the view is exact; it is locked and read
  first in the transaction of the view, only later changes are applied,
  and it is set to the end of the batch in the same transaction.
*/

static const char *base_name(const std::string &file) {
  return file.c_str() + dirname_length(file.c_str());
}

void Daemon::load_index_files() {
  index_files.clear();
  if (!mysql_bin_log.is_open()) return;
  Log_info info;
  mysql_bin_log.lock_index();
  int error = mysql_bin_log.find_log_pos(&info, NullS, false);
  while (!error) {
    index_files.push_back(
        info.log_file_name + dirname_length(info.log_file_name));
    error = mysql_bin_log.find_next_log(&info, false);
  }
  mysql_bin_log.unlock_index();
}

/* The place of a binary log file in the index, -1 when it is not there */
int Daemon::file_ordinal(const std::string &file) const {
  const char *name = base_name(file);
  for (size_t i = 0; i < index_files.size(); i++)
    if (index_files[i] == name) return static_cast<int>(i);
  return -1;
}

/* Whether the transaction of the row ends after a position */
bool Daemon::row_is_after(const Row_change &row, int ordinal,
                          my_off_t pos) const {
  if (row.trx >= trx_ordinals.size()) return true;
  const int row_ordinal = trx_ordinals[row.trx];
  if (row_ordinal != ordinal) return row_ordinal > ordinal || row_ordinal < 0;
  return batch.trx_ends[row.trx].second > pos;
}

/* Read (and with for_update, lock) the position of an aggregate view */
bool Daemon::read_view_position(size_t view, bool for_update,
                                Sql_error *error) {
  View_rt &v = rt[view];
  v.pos_valid = false;
  std::vector<std::string> row;
  std::vector<bool> nulls;
  if (exec_on(thd,
              "SELECT binlog_file, CAST(binlog_pos AS CHAR) FROM " + v.state_table +
                  " LIMIT 1" + (for_update ? " FOR UPDATE" : ""),
              error, &row, &nulls))
    return true;
  if (row.size() >= 2 && !nulls[0] && !nulls[1] && !row[0].empty()) {
    v.pos_file = base_name(row[0]);
    v.pos = strtoull(row[1].c_str(), nullptr, 10);
    v.pos_valid = true;
  }
  return false;
}

bool Daemon::position_usable(size_t view) const {
  return rt[view].pos_valid && file_ordinal(rt[view].pos_file) >= 0;
}

/* The rows of the batch that the view has not applied */
size_t Daemon::rows_after_position(size_t view) const {
  const View_rt &v = rt[view];
  const bool usable = position_usable(view);
  const int ordinal = usable ? file_ordinal(v.pos_file) : -1;
  std::set<std::string> seen;
  size_t count = 0;
  for (const Mview_table &table : views[view].tables) {
    const std::string key = table_key(table.db, table.table);
    if (!seen.insert(key).second) continue;
    auto changes = batch.changes.tables.find(key);
    if (changes == batch.changes.tables.end()) continue;
    for (const Row_change &row : changes->second.rows)
      if (!usable || row_is_after(row, ordinal, v.pos)) count++;
  }
  return count;
}

bool Daemon::reads_base_tables(size_t view) const {
  const Mview_def &def = views[view];
  return rt[view].force_pin || !def.delta_capable || def.tables.size() != 1;
}

bool Daemon::view_is_pinned(size_t view) const {
  if (snapshot_open &&
      std::find(snapshot_views.begin(), snapshot_views.end(), view) !=
          snapshot_views.end())
    return true;
  if (!pin_held) return false;
  for (const Mview_table &table : views[view].tables)
    if (!pinned_tables.count(table_key(table.db, table.table))) return false;
  return true;
}

/* The base tables that must be locked to apply the batch */
void Daemon::views_needing_pin(std::set<std::string> *tables) {
  if (!batch.has_end || !registry_loaded) return;
  load_index_files();
  trx_ordinals.clear();
  for (const auto &end : batch.trx_ends)
    trx_ordinals.push_back(file_ordinal(end.first));
  for (size_t i = 0; i < views.size(); i++) {
    if (join_where_needs_pin(i)) {
      for (const Mview_table &table : views[i].tables)
        tables->insert(table_key(table.db, table.table));
      continue;
    }
    if (!views[i].is_aggregate || !rt[i].maintained || rt[i].needs_refresh)
      continue;
    if (!reads_base_tables(i) || rows_after_position(i) == 0) continue;
    for (const Mview_table &table : views[i].tables)
      tables->insert(table_key(table.db, table.table));
  }
}

/* The allowance of lock time grows with the time that passes, by
   warp_mview_pin_duty_percent of it, up to what one second gives. */
bool Daemon::pin_allowed(std::chrono::steady_clock::duration *wait) {
  const auto now = std::chrono::steady_clock::now();
  const double rate = sv_pin_duty_percent / 100.0;
  const double cap = 1000000.0 * rate;
  if (!pin_budget_started) {
    pin_budget_started = true;
    pin_budget_us = cap;
  } else {
    pin_budget_us +=
        rate * std::chrono::duration_cast<std::chrono::microseconds>(
                   now - pin_budget_time)
                   .count();
    if (pin_budget_us > cap) pin_budget_us = cap;
  }
  pin_budget_time = now;
  *wait = std::chrono::steady_clock::duration::zero();
  if (pin_budget_us < 0 && rate < 1.0)
    *wait = std::chrono::microseconds(
        static_cast<long long>(-pin_budget_us / rate) + 1);
  if (now + *wait < pin_not_before) *wait = pin_not_before - now;
  return *wait == std::chrono::steady_clock::duration::zero();
}

void Daemon::spend_pin_time(std::chrono::steady_clock::duration spent) {
  if (sv_pin_duty_percent < 100)
    pin_budget_us -=
        std::chrono::duration_cast<std::chrono::microseconds>(spent).count();
  pin_not_before = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(sv_pin_min_interval);
}

/**
  Lock the tables for reading with the second session and note the end of
  the binary log: when the lock is granted no transaction that changed one
  of the tables is open, and none can start, so the tables are exactly as
  the binary log up to that position says.

  @retval true the tables are not locked (timeout, a table is gone)
*/
/*
  Shared read-only metadata locks on the base tables, as LOCK TABLES ...
  READ takes: they are granted when no transaction that changed a table is
  open, and no change starts while they are held.

  The daemon may wait for one table while it holds another, which can close
  a lock cycle with a transaction of a user (it changed table A and now
  changes B; the daemon has B and waits for A).  The session that holds the
  locks is the preferred victim of the deadlock detector
  (MDL_context::set_prefer_deadlock_victim), so it is always the daemon that
  gets the deadlock error, never the user: it releases everything and tries
  again later.
*/
bool Daemon::lock_base_tables(const std::set<std::string> &tables,
                              bool no_wait, std::string *why) {
  lock_thd->store_globals();
  MDL_context &context = lock_thd->mdl_context;
  context.set_prefer_deadlock_victim(true);
  if (no_wait) {
    /* Only when every table is free at this moment: nobody waits behind
       a request, so this costs writers nothing but the time the locks are
       held. */
    bool busy = false;
    for (const std::string &key : tables) {
      const size_t split = key.find('\0');
      MDL_request request;
      MDL_REQUEST_INIT(&request, MDL_key::TABLE, key.substr(0, split).c_str(),
                       key.substr(split + 1).c_str(), MDL_SHARED_READ_ONLY,
                       MDL_EXPLICIT);
      if (context.try_acquire_lock(&request) || request.ticket == nullptr) {
        busy = true;
        break;
      }
      pin_tickets.push_back(request.ticket);
    }
    if (busy) {
      lock_thd->clear_error();
      lock_thd->get_stmt_da()->reset_diagnostics_area();
      for (MDL_ticket *ticket : pin_tickets) context.release_lock(ticket);
      pin_tickets.clear();
      *why = "a base table is in use";
    }
    thd->store_globals();
    return busy;
  }
  std::vector<MDL_request> requests(tables.size());
  MDL_request_list list;
  size_t n = 0;
  for (const std::string &key : tables) {
    const size_t split = key.find('\0');
    /* the names are copied into the request */
    MDL_REQUEST_INIT(&requests[n], MDL_key::TABLE,
                     key.substr(0, split).c_str(),
                     key.substr(split + 1).c_str(), MDL_SHARED_READ_ONLY,
                     MDL_EXPLICIT);
    list.push_front(&requests[n]);
    n++;
  }
  /* all of them or none */
  bool failed = context.acquire_locks(
      &list, static_cast<Timeout_type>(sv_pin_lock_wait_timeout));
  if (failed) {
    Diagnostics_area *da = lock_thd->get_stmt_da();
    *why = da->is_error() ? da->message_text() : "interrupted";
    lock_thd->clear_error();
    da->reset_diagnostics_area();
  } else {
    for (const MDL_request &request : requests)
      pin_tickets.push_back(request.ticket);
  }
  thd->store_globals();
  return failed;
}

bool Daemon::acquire_pin(const std::set<std::string> &tables, bool no_wait) {
  std::string why;
  set_state("locking base tables");
  pin_requested = std::chrono::steady_clock::now();
  const bool failed = lock_base_tables(tables, no_wait, &why);
  pin_granted = std::chrono::steady_clock::now();
  if (failed && no_wait) return true; /* nobody waited: nothing to account */
  st_pin_wait_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                               pin_granted - pin_requested)
                               .count());
  if (failed) {
    absorb_kill();
    st_pin_failures.fetch_add(1);
    /* writers waited behind the request */
    spend_pin_time(pin_granted - pin_requested);
    if (!should_stop())
      log_limited(WARNING_LEVEL, "pin",
                  "cannot lock the base tables of materialized views for "
                  "reading, trying again later: " + why);
    return true;
  }
  thd->store_globals();
  pin_held = true;
  pinned_tables = tables;
  st_pins.fetch_add(1);

  /* LOCK_log: no commit group is being written */
  Log_info current;
  if (mysql_bin_log.get_current_log(&current, true) ||
      current.log_file_name[0] == 0) {
    release_pin();
    return true;
  }
  target_file = current.log_file_name;
  target_pos = current.pos;
  target_set = true;
  limit_set = false;
  return false;
}

void Daemon::unlock_base_tables() {
  if (!pin_held) return;
  lock_thd->store_globals();
  for (MDL_ticket *ticket : pin_tickets)
    lock_thd->mdl_context.release_lock(ticket);
  pin_tickets.clear();
  thd->store_globals();
  pin_held = false;
  const auto now = std::chrono::steady_clock::now();
  st_pin_lock_us.fetch_add(
      std::chrono::duration_cast<std::chrono::microseconds>(now - pin_granted)
          .count());
  spend_pin_time(now - pin_requested);
}

void Daemon::release_pin() {
  target_set = false;
  limit_set = false;
  if (snapshot_open) {
    exec("ROLLBACK");
    snapshot_open = false;
  }
  snapshot_views.clear();
  unlock_base_tables();
  pinned_tables.clear();
}

/**
  With the base tables locked: open a transaction with a consistent
  snapshot for the views whose base tables are all InnoDB.  The snapshot is
  the tables as of the position that was noted, so the lock is not needed
  any more for these views; it is released at once when no other view
  needs it.
*/
void Daemon::begin_snapshot() {
  snapshot_views.clear();
  bool others = false;
  for (size_t i = 0; i < views.size(); i++) {
    /* join views read with INSERT ... SELECT, which is not a snapshot
       read: they keep the lock */
    if (join_where_needs_pin(i)) others = true;
    if (!views[i].is_aggregate || !rt[i].maintained || rt[i].needs_refresh)
      continue;
    if (!reads_base_tables(i) || rows_after_position(i) == 0) continue;
    if (rt[i].snapshot_class && !rt[i].no_snapshot)
      snapshot_views.push_back(i);
    else
      others = true;
  }
  if (snapshot_views.empty()) return;
  Sql_error error;
  bool failed = false;
  for (size_t view : snapshot_views)
    failed = failed || ensure_agg_tables(view, &error);
  failed = failed || exec("COMMIT", &error) ||
           exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", &error) ||
           exec("START TRANSACTION WITH CONSISTENT SNAPSHOT", &error);
  if (failed) {
    exec("ROLLBACK");
    for (size_t view : snapshot_views) {
      rt[view].tmp_ready = false;
      rt[view].no_snapshot = true;
    }
    snapshot_views.clear();
    log_limited(WARNING_LEVEL, "snapshot" + std::to_string(error.code),
                "cannot open a consistent snapshot: " +
                    std::to_string(error.code) + " " + error.message);
    return;
  }
  snapshot_open = true;
  st_snapshots.fetch_add(1);
  if (!others) unlock_base_tables();
}

/**
  Apply the batch to the views of the snapshot, all in the transaction that
  has the snapshot (they are InnoDB tables: one commit).  When anything
  goes wrong the transaction is rolled back and the views are applied the
  slow way, with the tables locked, where errors are retried.
*/
Apply_result Daemon::apply_snapshot_group() {
  bool failed = false;
  Sql_error error;
  std::vector<size_t> advanced_views;
  for (size_t view : snapshot_views) {
    Agg_outcome outcome = Agg_outcome::DONE;
    std::string why;
    bool advanced = false;
    if (apply_agg_once(view, &error, &outcome, &why, false, &advanced) ||
        outcome != Agg_outcome::DONE) {
      failed = true;
      break;
    }
    if (advanced) advanced_views.push_back(view);
  }
  if (!failed && exec("COMMIT", &error)) failed = true;
  snapshot_open = false;
  if (failed) {
    rollback();
    for (size_t view : snapshot_views) {
      rt[view].no_snapshot = true;
      rt[view].tmp_ready = false;
    }
    snapshot_views.clear();
    return should_stop() ? Apply_result::STOP : Apply_result::REPIN;
  }
  for (size_t view : advanced_views) {
    rt[view].pos_file = base_name(batch.end_file);
    rt[view].pos = batch.end_pos;
    rt[view].pos_valid = true;
    rt[view].force_pin = false;
  }
  snapshot_views.clear();
  return Apply_result::OK;
}

/* While views wait for a pin: until the binary log grows, or a while */
void Daemon::wait_deferred(unsigned long milliseconds) {
  if (milliseconds == 0) return;
  if (reader && mysql_bin_log.is_active(linfo.log_file_name) &&
      reader->position() >= mysql_bin_log.get_binlog_end_pos()) {
    set_state("waiting for the binary log");
    wait_for_binlog(reader->position(), milliseconds);
  } else {
    pause(milliseconds < 5 ? milliseconds : 5);
  }
}

/* Identifies the requests of the batch to refresh views completely */
std::string Daemon::depends_mark() const {
  return std::to_string(batch.id) + ":" +
         std::to_string(batch.changes.refresh_tables.size()) +
         (batch.changes.refresh_all ? ":all" : "");
}

/* from_template with the sources of the table instances filled in */
static std::string substitute(const Mview_def &def,
                              const std::vector<std::string> &sources) {
  std::string out = def.from_template;
  for (size_t t = 0; t < sources.size(); t++) {
    const std::string token = "`mv$source$" + std::to_string(t) + "`";
    size_t at = 0;
    while ((at = out.find(token, at)) != std::string::npos) {
      out.replace(at, token.size(), sources[t]);
      at += sources[t].size();
    }
  }
  return out;
}

static std::vector<std::string> base_sources(const Mview_def &def) {
  std::vector<std::string> sources;
  for (const Mview_table &table : def.tables)
    sources.push_back(qname(table.db, table.table));
  return sources;
}

static std::string group_alias(size_t k) {
  return "`mvg" + std::to_string(k) + "`";
}

/* "(e0) AS `mvg0`, (e1) AS `mvg1`, " */
static std::string group_select(const Mview_def &def) {
  std::string out;
  for (size_t k = 0; k < def.group_exprs.size(); k++)
    out += "(" + def.group_exprs[k] + ") AS " + group_alias(k) + ", ";
  return out;
}

/* " GROUP BY `mvg0`, `mvg1`" */
static std::string group_by(const Mview_def &def) {
  std::string out;
  for (size_t k = 0; k < def.group_exprs.size(); k++)
    out += (k ? ", " : " GROUP BY ") + group_alias(k);
  return out;
}

/* "`mvg0`, `mvg1`, " */
static std::string group_aliases(const Mview_def &def,
                                 const std::string &qualifier) {
  std::string out;
  for (size_t k = 0; k < def.group_exprs.size(); k++)
    out += qualifier + group_alias(k) + ", ";
  return out;
}

/* The columns of the view that are written: groups, count, states */
static std::string view_columns(const Mview_def &def,
                                const std::string &qualifier) {
  std::string out;
  for (const std::string &column : def.view_group_columns)
    out += qualifier + qi(column) + ", ";
  out += qualifier + qi(def.count_column);
  for (const Mview_agg_state &state : def.states)
    out += ", " + qualifier + qi(state.column);
  return out;
}

/* "`mvv`.`g0` <=> `mvd`.`mvg0` AND ..." (TRUE without GROUP BY) */
static std::string group_match(const Mview_def &def) {
  if (def.group_exprs.empty()) return "TRUE";
  std::string out;
  for (size_t k = 0; k < def.group_exprs.size(); k++)
    out += std::string(k ? " AND " : "") + "`mvv`." +
           qi(def.view_group_columns[k]) + " <=> `mvd`." + group_alias(k);
  return out;
}

static std::string delta_column(size_t state) {
  return "`mvd_s" + std::to_string(state) + "`";
}
static std::string lost_column(size_t state) {
  return "`mvd_l" + std::to_string(state) + "`";
}

/**
  One term of the change of the groups: per group, the weighted row count
  and the weighted states of the rows of a join.

  @param weight  SQL of the weight of a row (+1 or -1)
  @param single  one table instance is its delta table: the rows of weight
                 +1 are rows of the join as it is now, so their values are
                 candidates for MIN and MAX.  With several delta tables
                 the rows are corrections, and every value says that the
                 MIN or MAX of the group has to be computed again.
*/
std::string Daemon::agg_term(size_t view, const std::string &from,
                             const std::string &weight, bool single) const {
  const Mview_def &def = views[view];
  std::string sql = "SELECT " + group_select(def) + "COALESCE(SUM(" + weight +
                    "), 0) AS `mvd_cnt`";
  for (size_t i = 0; i < def.states.size(); i++) {
    const Mview_agg_state &state = def.states[i];
    const std::string arg = "(" + state.arg_sql + ")";
    switch (state.kind) {
      case Mview_agg_state::COUNT_STAR:
        sql += ", COALESCE(SUM(" + weight + "), 0) AS " + delta_column(i);
        break;
      case Mview_agg_state::COUNT:
        sql += ", COALESCE(SUM(" + weight + " * (" + arg +
               " IS NOT NULL)), 0) AS " + delta_column(i);
        break;
      case Mview_agg_state::SUM:
        sql += ", COALESCE(SUM(" + weight + " * " + arg + "), 0) AS " +
               delta_column(i);
        break;
      case Mview_agg_state::MIN:
      case Mview_agg_state::MAX: {
        const char *func = state.kind == Mview_agg_state::MIN ? "MIN" : "MAX";
        if (single)
          sql += std::string(", ") + func + "(CASE WHEN " + weight +
                 " > 0 THEN " + arg + " END) AS " + delta_column(i) +
                 ", COALESCE(SUM(" + weight + " < 0 AND " + arg +
                 " IS NOT NULL), 0) AS " + lost_column(i);
        else
          sql += std::string(", ") + func + "(CASE WHEN FALSE THEN " + arg +
                 " END) AS " + delta_column(i) + ", COALESCE(SUM(" + arg +
                 " IS NOT NULL), 0) AS " + lost_column(i);
        break;
      }
      case Mview_agg_state::RECOMPUTE:
        break; /* not in delta capable views */
    }
  }
  sql += " " + from;
  if (!def.where_sql.empty()) sql += " WHERE (" + def.where_sql + ")";
  sql += group_by(def);
  return sql;
}

/*
  The temporary tables of an aggregate view (tmp_tables, in this order):
    delta table of every table instance
    gd  the terms of the group deltas        (delta capable)
    gs  the group deltas, one row per group  (delta capable)
    nv  the new rows of the changed groups   (delta capable)
    rg  the groups that are computed again   (with GROUP BY)
*/
/* A delta table: the referenced columns of a table instance and the sign
   of the image.  The columns come from the NULL side of an outer join:
   they keep their types and lose NOT NULL. */
bool Daemon::create_delta_table(const Mview_table &table,
                                const std::string &name, Sql_error *error) {
  std::string sql =
      "CREATE TEMPORARY TABLE " + name + " ENGINE=InnoDB AS SELECT ";
  for (const std::string &column : table.ref_columns)
    sql += "`mvb`." + qi(column) + ", ";
  sql += "`mvo`.`mv$sign` FROM (SELECT CAST(0 AS SIGNED) AS `mv$sign`) AS "
         "`mvo` LEFT JOIN " + qname(table.db, table.table) +
         " AS `mvb` ON FALSE LIMIT 0";
  return exec(sql, error);
}

bool Daemon::ensure_agg_tables(size_t view, Sql_error *error) {
  View_rt &v = rt[view];
  if (v.tmp_ready) return false;
  const Mview_def &def = views[view];
  const size_t n = def.tables.size();
  for (const std::string &table : v.tmp_tables)
    exec("DROP TEMPORARY TABLE IF EXISTS " + table);

  for (size_t t = 0; t < n; t++) {
    const Mview_table &table = def.tables[t];
    if (create_delta_table(table, v.tmp_tables[t], error)) return true;
  }
  const std::string &gd = v.tmp_tables[n], &gs = v.tmp_tables[n + 1],
                    &nv = v.tmp_tables[n + 2], &rg = v.tmp_tables[n + 3];
  if (def.delta_capable) {
    if (exec("CREATE TEMPORARY TABLE " + gd + " ENGINE=InnoDB AS " +
                 agg_term(view, substitute(def, base_sources(def)),
                          "CAST(1 AS SIGNED)", true) +
                 " LIMIT 0",
             error) ||
        exec("CREATE TEMPORARY TABLE " + gs + " ENGINE=InnoDB AS SELECT * FROM " +
                 gd + " LIMIT 0",
             error) ||
        exec("CREATE TEMPORARY TABLE " + nv + " ENGINE=InnoDB AS SELECT " +
                 view_columns(def, "") + " FROM " + v.name + " LIMIT 0",
             error))
      return true;
  }
  if (!def.group_exprs.empty()) {
    std::string select = group_select(def);
    select.resize(select.size() - 2); /* ", " */
    if (exec("CREATE TEMPORARY TABLE " + rg + " ENGINE=InnoDB AS SELECT " +
                 select + " " + def.from_sql + " LIMIT 0",
             error))
      return true;
  }
  v.tmp_ready = true;
  return false;
}

/**
  Load the images of the changed rows of a table instance that the view has
  not applied into the delta table of the instance.

  @param[out] rows     number of images loaded
  @param[out] why_not  set when the images cannot be used (a referenced
                       column is not in an image, or has no literal)
*/
bool Daemon::fill_delta(size_t view, size_t instance,
                        const std::string &delta, bool old_only, int ordinal,
                        my_off_t pos, unsigned from_trx, size_t *rows,
                        std::string *why_not, Sql_error *error) {
  const Mview_table &table = views[view].tables[instance];
  *rows = 0;
  auto changes = batch.changes.tables.find(table_key(table.db, table.table));
  if (changes == batch.changes.tables.end()) return false;

  std::vector<int> slots;
  std::string head = "INSERT INTO " + delta + " (";
  for (size_t k = 0; k < table.ref_columns.size(); k++) {
    const int slot = changes->second.slot(table.ref_ordinals[k]);
    if (slot < 0) {
      *why_not = "column " + table.ref_columns[k] + " of " + table.table +
                 " was not read from the binary log";
      return false;
    }
    slots.push_back(slot);
    head += qi(table.ref_columns[k]) + ", ";
  }
  head += "`mv$sign`) VALUES ";

  const size_t chunk = sv_in_list_chunk_size ? sv_in_list_chunk_size : 1;
  std::string sql;
  size_t pending = 0;
  for (const Row_change &row : changes->second.rows) {
    if (row.trx < from_trx || !row_is_after(row, ordinal, pos)) continue;
    const bool has_old = row.kind != Row_change::INSERT;
    const bool has_new = row.kind != Row_change::DELETE && !old_only;
    if (has_old && row.kind == Row_change::UPDATE) {
      /* an update that changes no referenced column changes no group */
      bool same = true;
      for (int slot : slots) {
        const size_t s = static_cast<size_t>(slot);
        if (s >= row.before.size() || s >= row.after.size() ||
            row.before[s].state != row.after[s].state ||
            row.before[s].literal != row.after[s].literal) {
          same = false;
          break;
        }
      }
      if (same && !slots.empty() &&
          row.before[slots[0]].state != Cell_state::ABSENT)
        continue;
    }
    for (int image = 0; image < 2; image++) {
      if (image == 0 ? !has_old : !has_new) continue;
      const std::vector<Cell> &cells = image == 0 ? row.before : row.after;
      std::string tuple = "(";
      for (size_t k = 0; k < slots.size(); k++) {
        const size_t s = static_cast<size_t>(slots[k]);
        if (s >= cells.size() || cells[s].state == Cell_state::ABSENT) {
          *why_not = "column " + table.ref_columns[k] + " of " + table.table +
                     " is not in a row image: aggregate materialized views "
                     "need binlog_row_image=FULL";
          return false;
        }
        if (cells[s].state == Cell_state::UNSUPPORTED) {
          *why_not = "column " + table.ref_columns[k] + " of " + table.table +
                     ": " + cells[s].literal;
          return false;
        }
        tuple += cells[s].literal + ",";
      }
      tuple += image == 0 ? "-1)" : "1)";
      if (pending == 0)
        sql = head;
      else
        sql.push_back(',');
      sql += tuple;
      (*rows)++;
      if (++pending == chunk) {
        if (exec(sql, error)) return true;
        pending = 0;
      }
    }
  }
  if (pending > 0 && exec(sql, error)) return true;
  return false;
}

/* Compute the groups of rg (or the one row of a view without GROUP BY)
   from the base tables and insert them into the view. */
bool Daemon::recompute_groups(size_t view, Sql_error *error) {
  const Mview_def &def = views[view];
  const View_rt &v = rt[view];
  std::string sql = "SELECT " + group_select(def) + "COUNT(*)";
  for (const Mview_agg_state &state : def.states)
    sql += ", " + state.full_sql;
  sql += " " + def.from_sql;
  std::string where;
  if (!def.where_sql.empty()) where = "(" + def.where_sql + ")";
  if (!def.group_exprs.empty()) {
    if (!where.empty()) where += " AND ";
    where += "EXISTS (SELECT 1 FROM " + v.tmp_tables[def.tables.size() + 3] +
             " AS `mvr` WHERE ";
    for (size_t k = 0; k < def.group_exprs.size(); k++)
      where += std::string(k ? " AND " : "") + "`mvr`." + group_alias(k) +
               " <=> (" + def.group_exprs[k] + ")";
    where += ")";
  }
  if (!where.empty()) sql += " WHERE " + where;
  sql += group_by(def);
  return copy_rows(sql, def.group_exprs.size() + 1 + def.states.size(),
                   "INSERT INTO " + v.name + " (" + view_columns(def, "") +
                       ") VALUES ",
                   error);
}

/**
  Delta capable views: add the changes of the groups.

    gd := for every non-empty subset U of the changed instances, the join
          with the delta tables of U, weighted (-1)^(|U|+1) * signs
    gs := gd summed by group
    groups whose MIN or MAX may have lost its value go to rg
    nv := the other groups of gs with their new count and states, from the
          old row of the view (or from nothing for a new group)
    delete the groups of gs from the view, insert nv (without the groups
    whose count is 0, when there is a GROUP BY), compute the groups of rg
    from the base tables.
*/
bool Daemon::apply_delta(size_t view, const std::vector<size_t> &changed,
                         Sql_error *error, Agg_outcome *outcome) {
  const Mview_def &def = views[view];
  const View_rt &v = rt[view];
  const size_t n = def.tables.size();
  const std::string &gd = v.tmp_tables[n], &gs = v.tmp_tables[n + 1],
                    &nv = v.tmp_tables[n + 2], &rg = v.tmp_tables[n + 3];
  const bool grouped = !def.group_exprs.empty();
  if (changed.size() > 4) {
    *outcome = Agg_outcome::NEED_REFRESH;
    return false;
  }

  for (unsigned mask = 1; mask < (1U << changed.size()); mask++) {
    std::vector<std::string> sources = base_sources(def);
    std::string weight;
    unsigned bits = 0;
    for (size_t c = 0; c < changed.size(); c++) {
      if (!(mask & (1U << c))) continue;
      bits++;
      sources[changed[c]] = v.tmp_tables[changed[c]];
      weight += " * " + qi(def.tables[changed[c]].alias) + ".`mv$sign`";
    }
    weight = std::string("(") + (bits % 2 ? "1" : "-1") + weight + ")";
    const std::string term =
        agg_term(view, substitute(def, sources), weight, bits == 1);
    if (bits < n) {
      /* base tables are read: by a plain SELECT (see copy_rows) */
      size_t columns = def.group_exprs.size() + 1;
      for (const Mview_agg_state &state : def.states)
        columns += state.kind == Mview_agg_state::MIN ||
                           state.kind == Mview_agg_state::MAX
                       ? 2
                       : 1;
      if (copy_rows(term, columns, "INSERT INTO " + gd + " VALUES ", error))
        return true;
    } else if (exec("INSERT INTO " + gd + " " + term, error)) {
      return true;
    }
  }

  std::string sum = "INSERT INTO " + gs + " SELECT " + group_aliases(def, "") +
                    "SUM(`mvd_cnt`)";
  std::string lost;
  for (size_t i = 0; i < def.states.size(); i++) {
    const Mview_agg_state::Kind kind = def.states[i].kind;
    if (kind == Mview_agg_state::MIN || kind == Mview_agg_state::MAX) {
      sum += std::string(", ") + (kind == Mview_agg_state::MIN ? "MIN" : "MAX") +
             "(" + delta_column(i) + "), SUM(" + lost_column(i) + ")";
      lost += std::string(lost.empty() ? "" : " OR ") + "`mvd`." +
              lost_column(i) + " > 0";
    } else {
      sum += ", SUM(" + delta_column(i) + ")";
    }
  }
  sum += " FROM " + gd + group_by(def);
  if (exec(sum, error)) return true;

  long long lost_groups = 0;
  if (!lost.empty() &&
      exec("SELECT CAST(COUNT(*) AS CHAR) FROM " + gs + " AS `mvd` WHERE " + lost, error,
           &lost_groups))
    return true;
  if (lost_groups > 0 && !view_is_pinned(view)) {
    /* the base tables are read: they must be locked */
    *outcome = Agg_outcome::NEED_PIN;
    return false;
  }
  const std::string kept = lost.empty() ? "TRUE" : "NOT (" + lost + ")";
  if (lost_groups > 0 && grouped) {
    std::string groups = group_aliases(def, "`mvd`.");
    groups.resize(groups.size() - 2); /* ", " */
    if (exec("INSERT INTO " + rg + " SELECT " + groups + " FROM " + gs +
                 " AS `mvd` WHERE " + lost,
             error))
      return true;
  }

  /* The new rows are computed from the old ones before anything is
     changed: no expression depends on the order of assignments. */
  std::string existing = "INSERT INTO " + nv + " SELECT ";
  std::string fresh = "INSERT INTO " + nv + " SELECT ";
  for (const std::string &column : def.view_group_columns)
    existing += "`mvv`." + qi(column) + ", ";
  fresh += group_aliases(def, "`mvd`.");
  existing += "`mvv`." + qi(def.count_column) + " + `mvd`.`mvd_cnt`";
  fresh += "`mvd`.`mvd_cnt`";
  for (size_t i = 0; i < def.states.size(); i++) {
    const Mview_agg_state &state = def.states[i];
    const std::string old = "`mvv`." + qi(state.column);
    const std::string delta = "`mvd`." + delta_column(i);
    switch (state.kind) {
      case Mview_agg_state::SUM: {
        /* NULL when no row has a value: by the COUNT state of the
           argument */
        const size_t cs = static_cast<size_t>(state.count_state);
        const std::string count_delta = "`mvd`." + delta_column(cs);
        existing += ", IF(`mvv`." + qi(def.states[cs].column) + " + " +
                    count_delta + " = 0, NULL, COALESCE(" + old + ", 0) + " +
                    delta + ")";
        fresh += ", IF(" + count_delta + " = 0, NULL, " + delta + ")";
        break;
      }
      case Mview_agg_state::MIN:
      case Mview_agg_state::MAX:
        existing += ", CASE WHEN " + old + " IS NULL THEN " + delta +
                    " WHEN " + delta + " IS NULL THEN " + old + " ELSE " +
                    (state.kind == Mview_agg_state::MIN ? "LEAST" : "GREATEST") +
                    "(" + old + ", " + delta + ") END";
        fresh += ", " + delta;
        break;
      default: /* COUNT, COUNT_STAR */
        existing += ", " + old + " + " + delta;
        fresh += ", " + delta;
        break;
    }
  }
  const std::string match = group_match(def);
  existing += " FROM " + v.name + " AS `mvv` JOIN " + gs + " AS `mvd` ON " +
              match + " WHERE " + kept;
  fresh += " FROM " + gs + " AS `mvd` WHERE " + kept +
           " AND NOT EXISTS (SELECT 1 FROM " + v.name + " AS `mvv` WHERE " +
           match + ")";
  if (exec(existing, error) || exec(fresh, error)) return true;

  if (exec("DELETE `mvv` FROM " + v.name + " AS `mvv` JOIN " + gs +
               " AS `mvd` ON " + match,
           error))
    return true;
  /* A view without GROUP BY always has its one row. */
  if (exec("INSERT INTO " + v.name + " (" + view_columns(def, "") +
               ") SELECT " + view_columns(def, "") + " FROM " + nv +
               (grouped ? " WHERE " + qi(def.count_column) + " <> 0" : ""),
           error))
    return true;
  if (lost_groups > 0 && recompute_groups(view, error)) return true;
  return false;
}

/**
  Other aggregate views: find the groups that may have changed, delete
  them and compute them from the base tables.

  The groups are those of the rows of the join in which, for one changed
  instance T in turn, T is its delta table (old and new images), every
  other changed instance is its base table or the old images of its delta
  table, and every NULL-supplying instance is its base table or empty (for
  the NULL-extended rows).  The WHERE of the view is not applied: more
  groups than needed do no harm.
*/
bool Daemon::apply_recompute(size_t view, const std::vector<size_t> &changed,
                             Sql_error *error, Agg_outcome *outcome) {
  const Mview_def &def = views[view];
  const View_rt &v = rt[view];
  const size_t n = def.tables.size();
  const std::string &rg = v.tmp_tables[n + 3];

  if (def.group_exprs.empty()) {
    if (exec("DELETE FROM " + v.name, error)) return true;
    return recompute_groups(view, error);
  }

  /* the choices of every instance */
  std::vector<bool> is_changed(n, false);
  for (size_t c : changed) is_changed[c] = true;
  std::vector<std::string> statements;
  std::string select = group_select(def);
  select.resize(select.size() - 2);
  for (size_t target : changed) {
    std::vector<std::vector<std::string>> options(n);
    size_t combinations = 1;
    for (size_t t = 0; t < n; t++) {
      const Mview_table &table = def.tables[t];
      if (t == target) {
        options[t].push_back(v.tmp_tables[t]);
      } else {
        options[t].push_back(qname(table.db, table.table));
        if (is_changed[t])
          options[t].push_back("(SELECT * FROM " + v.tmp_tables[t] +
                               " WHERE `mv$sign` < 0)");
        if (table.null_supplying)
          options[t].push_back("(SELECT * FROM " +
                               qname(table.db, table.table) + " WHERE FALSE)");
      }
      combinations *= options[t].size();
    }
    if (statements.size() + combinations > 64) {
      *outcome = Agg_outcome::NEED_REFRESH;
      return false;
    }
    std::vector<size_t> choice(n, 0);
    for (size_t c = 0; c < combinations; c++) {
      std::vector<std::string> sources(n);
      for (size_t t = 0; t < n; t++) sources[t] = options[t][choice[t]];
      statements.push_back("SELECT DISTINCT " + select + " " +
                           substitute(def, sources));
      for (size_t t = 0; t < n; t++) {
        if (++choice[t] < options[t].size()) break;
        choice[t] = 0;
      }
    }
  }
  for (const std::string &statement : statements)
    if (copy_rows(statement, def.group_exprs.size(),
                  "INSERT INTO " + rg + " VALUES ", error))
      return true;

  std::string match;
  for (size_t k = 0; k < def.group_exprs.size(); k++)
    match += std::string(k ? " AND " : "") + "`mvv`." +
             qi(def.view_group_columns[k]) + " <=> `mvd`." + group_alias(k);
  if (exec("DELETE `mvv` FROM " + v.name + " AS `mvv` JOIN (SELECT DISTINCT * "
           "FROM " + rg + ") AS `mvd` ON " + match,
           error))
    return true;
  return recompute_groups(view, error);
}

/**
  One attempt to apply the batch to an aggregate view, in one transaction:

    begin
    lock and read the position of the view; changes up to it are skipped
    load the delta tables
    change the groups (apply_delta or apply_recompute)
    set the position of the view to the end of the batch
    commit

  @param[out] outcome  DONE: committed.  NEED_PIN: the base tables must be
                       locked first.  NEED_REFRESH: the view cannot be
                       updated from this batch (*why).  In both cases the
                       caller rolls back.
  @retval true  SQL error; the caller rolls back
*/
bool Daemon::apply_agg_once(size_t view, Sql_error *error,
                            Agg_outcome *outcome, std::string *why,
                            bool commit, bool *advanced) {
  const Mview_def &def = views[view];
  View_rt &v = rt[view];
  const size_t n = def.tables.size();
  *outcome = Agg_outcome::DONE;
  *advanced = false;

  mview_set_maintenance_mode(thd, true);
  /* a multi-table DELETE resolves its target alias in the current schema */
  if (exec("USE " + qi(def.db), error)) return true;
  /* -- begin (autocommit is off) -- */
  if (ensure_agg_tables(view, error)) return true;
  /* The metadata lock of the view first: a REFRESH that is running ends
     before the position is read, and none starts before the commit. */
  if (exec("SELECT 1 FROM " + v.name + " LIMIT 0", error)) return true;
  if (read_view_position(view, true, error)) return true;
  if (!position_usable(view)) {
    *outcome = Agg_outcome::NEED_REFRESH;
    *why = "its recorded binary log position is not in the binary log";
    return false;
  }
  const int ordinal = file_ordinal(v.pos_file);
  const int end_ordinal = file_ordinal(batch.end_file);
  if (ordinal > end_ordinal ||
      (ordinal == end_ordinal && v.pos >= batch.end_pos)) {
    /* refreshed after the batch: it has all of it */
    return commit ? exec("COMMIT", error) : false;
  }

  for (size_t t = 0; t < v.tmp_tables.size(); t++) {
    const bool exists = t < n || (t < n + 3 && def.delta_capable) ||
                        (t == n + 3 && !def.group_exprs.empty());
    if (exists && exec("DELETE FROM " + v.tmp_tables[t], error)) return true;
  }
  std::vector<size_t> changed;
  size_t images = 0;
  for (size_t t = 0; t < n; t++) {
    size_t rows = 0;
    if (fill_delta(view, t, v.tmp_tables[t], false, ordinal, v.pos, 0, &rows,
                   why, error))
      return true;
    if (!why->empty()) {
      *outcome = Agg_outcome::NEED_REFRESH;
      return false;
    }
    if (rows > 0) changed.push_back(t);
    images += rows;
  }

  if (!changed.empty()) {
    if (reads_base_tables(view) && !view_is_pinned(view)) {
      *outcome = Agg_outcome::NEED_PIN;
      return false;
    }
    if (def.delta_capable ? apply_delta(view, changed, error, outcome)
                          : apply_recompute(view, changed, error, outcome))
      return true;
    if (*outcome != Agg_outcome::DONE) {
      if (*outcome == Agg_outcome::NEED_REFRESH)
        *why = "too many tables changed at once";
      return false;
    }
  }

  /* -- record the position -- */
  if (exec("UPDATE " + v.state_table + " SET binlog_file = _utf8mb4 X'" +
               to_hex(base_name(batch.end_file)) + "', binlog_pos = " +
               std::to_string(batch.end_pos),
           error))
    return true;
  *advanced = true;
  st_keys.fetch_add(images);
  if (!commit) return false; /* the caller commits and notes the position */
  /* -- commit -- */
  if (exec("COMMIT", error)) return true;
  v.pos_file = base_name(batch.end_file);
  v.pos = batch.end_pos;
  v.pos_valid = true;
  v.force_pin = false;
  v.no_snapshot = false;
  return false;
}

Apply_result Daemon::apply_agg_view(size_t view) {
  View_rt &v = rt[view];
  unsigned attempt = 0;
  for (;;) {
    Sql_error error;
    Agg_outcome outcome = Agg_outcome::DONE;
    std::string why;
    bool advanced = false;
    if (!apply_agg_once(view, &error, &outcome, &why, true, &advanced)) {
      if (outcome == Agg_outcome::DONE) return Apply_result::OK;
      rollback();
      if (should_stop()) return Apply_result::STOP;
      if (outcome == Agg_outcome::NEED_PIN) {
        v.force_pin = true;
        return Apply_result::REPIN;
      }
      log_limited(WARNING_LEVEL, v.name + ": " + why,
                  "materialized view " + v.name +
                      " cannot be updated from the binary log (" + why +
                      "); it is refreshed completely instead");
      return refresh_view(view, why.c_str());
    }
    v.tmp_ready = false; /* made again by the next attempt */
    bool give_up = false;
    const Apply_result next =
        after_error(error, &attempt, &give_up, "the update", v.name);
    if (next != Apply_result::OK) return next;
    if (give_up)
      return refresh_view(view, "its changes could not be applied");
  }
}

/**
  Apply the batch to every maintained view and save the position.

  OK       applied, position saved, batch cleared
  STOP     the daemon stops; the batch is read again after a restart
  RESTART  apply the same batch again (the registry is loaded first)
*/
Apply_result Daemon::apply_batch(bool allow_defer) {
  set_state("applying changes");
  st_caught_up.store(false);

  /* Views that depend on tables that DDL or undecodable rows changed, by
     the definitions that were current when the batch was read. */
  const std::set<std::string> &refresh_tables = batch.changes.refresh_tables;
  std::set<std::string> refresh_views;
  for (size_t i = 0; i < views.size(); i++) {
    if (!rt[i].maintained && !rt[i].needs_refresh) continue;
    for (const Mview_table &table : views[i].tables)
      if (refresh_tables.count(table_key(table.db, table.table)))
        refresh_views.insert(rt[i].name);
  }

  if (batch.reload || !registry_loaded ||
      mview_registry_version.load() != registry_version) {
    if (snapshot_open || pin_held) {
      /* not with a snapshot or locks: the pin is for the views as they
         were */
      release_pin();
      return Apply_result::RESTART;
    }
    /* A view that is in the old definitions and cannot be loaded any more
       becomes stale in load_registry(). */
    if (load_registry()) {
      pause(1000);
      return should_stop() ? Apply_result::STOP : Apply_result::RESTART;
    }
    batch.reload = false;
  }

  load_index_files();
  trx_ordinals.clear();
  for (const auto &end : batch.trx_ends)
    trx_ordinals.push_back(file_ordinal(end.first));

  /* the views of the snapshot first: its transaction is open */
  if (snapshot_open) {
    const Apply_result result = apply_snapshot_group();
    if (result != Apply_result::OK) return result;
  }

  std::map<std::string, Key_sets> keys;
  make_key_sets(&keys, batch.join_applied_trx);
  join_from_trx = batch.join_applied_trx;
  const std::string mark = depends_mark();

  /* With the base tables locked: first the views that need the lock, then
     the lock is released and the others follow. */
  std::vector<size_t> order;
  size_t locked_views = 0;
  if (pin_held) {
    for (size_t i = 0; i < views.size(); i++)
      if (views[i].is_aggregate && rt[i].maintained && !rt[i].needs_refresh &&
          position_usable(i) && reads_base_tables(i) && view_is_pinned(i) &&
          rows_after_position(i) > 0 &&
          rows_after_position(i) <= sv_full_refresh_threshold)
        order.push_back(i);
    for (size_t i = 0; i < views.size(); i++)
      if (join_where_needs_pin(i) && view_is_pinned(i)) order.push_back(i);
    locked_views = order.size();
  }
  for (size_t i = 0; i < views.size(); i++)
    if (std::find(order.begin(), order.begin() + locked_views, i) ==
        order.begin() + locked_views)
      order.push_back(i);

  bool view_work = false;
  bool deferred = false;
  for (size_t n = 0; n < order.size(); n++) {
    const size_t i = order[n];
    if (n == locked_views) unlock_base_tables();
    View_rt &v = rt[i];
    const Mview_def &def = views[i];
    if (!v.maintained && !v.needs_refresh) continue;
    if (should_stop()) return Apply_result::STOP;
    if (!sv_enabled) return Apply_result::RESTART;

    bool depends = batch.changes.refresh_all || refresh_views.count(v.name);
    bool bad_keys = false;
    size_t key_count = 0;
    for (const Mview_table &table : def.tables) {
      const std::string key = table_key(table.db, table.table);
      if (refresh_tables.count(key)) depends = true;
      auto sets = keys.find(key_set_id(table));
      if (sets != keys.end()) {
        if (sets->second.bad) bad_keys = true;
        key_count += sets->second.size();
      }
    }
    /* no root keys to recompute: a root without row key columns */
    if (!def.is_aggregate && key_count > 0 &&
        def.tables[def.root].pk_columns.empty())
      bad_keys = true;
    /* already refreshed for these requests while applying this batch */
    if (depends) {
      auto done = refreshed.find(v.name);
      if (done != refreshed.end() && done->second == mark) depends = false;
    }

    Apply_result result = Apply_result::OK;
    if (v.needs_refresh) {
      result = refresh_view(i, "it was not followed by the daemon until now");
    } else if (depends) {
      result = refresh_view(
          i, "a base table was changed in a way that has no row events");
    } else if (def.is_aggregate) {
      if (!batch.has_end) continue;
      if (!position_usable(i)) {
        result = refresh_view(
            i, "its recorded binary log position is not in the binary log");
      } else {
        const size_t rows = rows_after_position(i);
        /* Also when the reader is in another file than the position of
           the view: the position follows the reader, so that the file it
           names is not purged. */
        if (rows == 0 && v.pos_file == base_name(batch.end_file)) continue;
        if (rows > sv_full_refresh_threshold) {
          result = refresh_view(i, "more changed rows than "
                                   "warp_mview_full_refresh_threshold");
        } else if (allow_defer && rows > 0 && reads_base_tables(i) &&
                   !view_is_pinned(i)) {
          deferred = true; /* waits for the base tables to be locked */
          continue;
        } else {
          result = apply_agg_view(i);
          if (result == Apply_result::REPIN && allow_defer) {
            deferred = true; /* a MIN or MAX has to be computed again */
            continue;
          }
        }
      }
    } else if (v.join_where) {
      /* its own part of the batch, see join_where_start() */
      std::map<std::string, Key_sets> own_keys;
      make_key_sets(&own_keys, join_where_start(i));
      bad_keys = false;
      key_count = 0;
      for (const Mview_table &table : def.tables) {
        auto sets = own_keys.find(key_set_id(table));
        if (sets == own_keys.end()) continue;
        if (sets->second.bad) bad_keys = true;
        key_count += sets->second.size();
      }
      if (key_count > 0 && def.tables[def.root].pk_columns.empty())
        bad_keys = true;
      if (bad_keys) {
        result = refresh_view(i, "the row keys of changed rows cannot "
                                 "be used");
      } else if (key_count == 0) {
        join_where_done(i);
        continue;
      } else if (key_count > sv_full_refresh_threshold) {
        result = refresh_view(i, "more changed keys than "
                                 "warp_mview_full_refresh_threshold");
      } else if (join_where_needs_pin(i) && !view_is_pinned(i)) {
        if (!allow_defer) return Apply_result::REPIN;
        deferred = true; /* waits for the base tables to be locked */
        continue;
      } else {
        join_from_trx = join_where_start(i);
        result = apply_view(i, own_keys);
        join_from_trx = batch.join_applied_trx;
      }
      if (result == Apply_result::OK) join_where_done(i);
    } else if (bad_keys) {
      result = refresh_view(i, "the row keys of changed rows cannot be "
                               "used");
    } else if (key_count == 0) {
      continue;
    } else if (key_count > sv_full_refresh_threshold) {
      result = refresh_view(i, "more changed keys than "
                               "warp_mview_full_refresh_threshold");
    } else {
      result = apply_view(i, keys);
    }
    if (result == Apply_result::STOP || result == Apply_result::RESTART ||
        result == Apply_result::REPIN)
      return result;
    view_work = true;
    /* FAILED: the view is stale now; the others go on */
  }

  /* the join views have everything that was read */
  batch.join_applied_trx = static_cast<unsigned>(batch.trx_ends.size());
  if (deferred) {
    /* The batch stays, for the views that wait; more is read into it. */
    batch_deferred = true;
    return Apply_result::OK;
  }
  batch_deferred = false;

  /* Everything up to the end of the batch is in the views: the reader
     restarts from here.  (Aggregate views that are ahead of it skip what
     they have, join views are recomputed again, which is harmless.) */
  if (batch.has_end) {
    saved_file = batch.end_file;
    saved_pos = batch.end_pos;
    have_saved_position = true;
    mview_set_applied_position(saved_file, saved_pos);
  }
  /* The file is synced: for batches that changed no view, at most once a
     second (and when the daemon stops). */
  const auto now = std::chrono::steady_clock::now();
  if (view_work || now - last_state_save >= std::chrono::seconds(1)) {
    save_state();
    last_state_save = now;
    state_dirty = false;
  } else {
    state_dirty = true;
  }
  st_batches.fetch_add(1);
  st_transactions.fetch_add(batch.transactions);
  st_rows.fetch_add(batch.changes.rows);
  st_last_applied_ts.store(st_last_read_ts.load());
  batch.clear();
  refreshed.clear();
  logged_undecodable.clear();
  return Apply_result::OK;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

void Daemon::run(const void *stack_base) {
  /* Plugins are initialized before the data dictionary and the binary log
     are usable: wait until the server accepts connections. */
  set_state("waiting for the server to start");
  while (!stop_requested.load() && get_server_state() == SERVER_BOOTING)
    my_sleep(50000);
  if (stop_requested.load() || opt_initialize ||
      get_server_state() != SERVER_OPERATING) {
    set_state("stopped");
    return;
  }

  /* INSTALL PLUGIN starts this thread from the init function of the
     plugin and then, still holding LOCK_plugin, sets the persisted system
     variables of plugins, which takes LOCK_global_system_variables.
     Creating a THD takes the same two mutexes in the opposite order
     (plugin_thdvar_init): wait until the statement that installs the
     plugin has ended.  Only tried, never waited for: UNINSTALL PLUGIN
     holds the mutex while it waits for this thread to end. */
  set_state("waiting for INSTALL PLUGIN to end");
  for (;;) {
    if (stop_requested.load()) {
      set_state("stopped");
      return;
    }
    if (mysql_mutex_trylock(&LOCK_plugin_install) == 0) {
      mysql_mutex_unlock(&LOCK_plugin_install);
      break;
    }
    my_sleep(10000);
  }

  if (create_session(stack_base)) {
    wmv_log(ERROR_LEVEL, "cannot create the session of the daemon");
    set_state("stopped");
    return;
  }
  setup_session();
  load_state();
  wmv_log(INFORMATION_LEVEL, "materialized view daemon started");

  while (!should_stop()) {
    absorb_kill();
    if (!sv_enabled) {
      set_state("paused");
      pause(200);
      continue;
    }
    if (check_requirements()) {
      close_reader();
      pause(5000);
      continue;
    }
    if (!positioned) {
      if (init_position()) {
        pause(1000);
        continue;
      }
    }
    if (!registry_loaded ||
        (mview_registry_version.load() != registry_version &&
         !batch.has_work())) {
      if (load_registry()) {
        pause(1000);
        continue;
      }
    }

    bool refresh_pending = false;
    for (const View_rt &view : rt)
      refresh_pending = refresh_pending || view.needs_refresh;

    /* Read: waits when there is nothing to do; with a batch that waits
       for a pin it only takes what is there. */
    std::string why;
    bool read_failed = false;
    if (batch.has_work() || !refresh_pending) {
      const Read_result result = read_batch(&why);
      if (result == Read_result::STOP) break;
      read_failed = result == Read_result::FAILED;
      if (result == Read_result::IDLE) {
        /* the position moves on over transactions without tracked rows */
        if (!batch.has_work() && batch.has_end && !in_trx &&
            (batch.end_file != saved_file || batch.end_pos != saved_pos)) {
          saved_file = batch.end_file;
          saved_pos = batch.end_pos;
          state_dirty = true;
          mview_set_applied_position(saved_file, saved_pos);
          batch.clear();
        }
        if (state_dirty && std::chrono::steady_clock::now() - last_state_save >=
                               std::chrono::seconds(1)) {
          save_state();
          last_state_save = std::chrono::steady_clock::now();
          state_dirty = false;
        }
        if (!batch.has_work()) continue;
      }
    }

    /* Aggregate views that are computed from their base tables: lock the
       tables (not more often than the allowance permits), read the binary
       log up to its end under the lock, and apply everything.  Until
       then the other views get what was read. */
    bool pinned = false;
    std::chrono::steady_clock::duration pin_wait{};
    if (!read_failed) {
      std::set<std::string> pin_tables;
      if (batch.has_work() &&
          mview_registry_version.load() == registry_version && !batch.reload)
        views_needing_pin(&pin_tables);
      /* Over the allowance of lock time the tables are still locked when
         they are free right now (no writer waits for that): the views
         catch up as soon as the writers pause. */
      const bool pin_paid =
          !pin_tables.empty() && pin_allowed(&pin_wait);
      if (!pin_tables.empty()) {
        if (acquire_pin(pin_tables, !pin_paid)) {
          if (should_stop()) break;
          if (pin_paid) pin_allowed(&pin_wait);
        } else {
          begin_snapshot();
          const Read_result result = read_batch(&why);
          const bool at_target =
              result == Read_result::BATCH && batch.has_end && !in_trx &&
              compare_log_name(batch.end_file.c_str(), target_file.c_str()) ==
                  0 &&
              batch.end_pos == target_pos;
          if (at_target) {
            pinned = true;
          } else {
            release_pin();
            if (result == Read_result::STOP) break;
            read_failed = result == Read_result::FAILED;
          }
        }
      }
    }
    if (read_failed) {
      /* e.g. RESET BINARY LOGS AND GTIDS: try the position of the last
         complete transaction again, then give the position up */
      read_failures++;
      wmv_log(WARNING_LEVEL, "%s", why.c_str());
      discard_transaction();
      if (read_failures > 3 ||
          open_reader(batch.has_end ? batch.end_file.c_str()
                                    : saved_file.c_str(),
                      batch.has_end ? batch.end_pos : saved_pos, &why))
        lost_position(why.c_str());
      else
        pause(200);
      continue;
    }

    /* a pass without a pin has nothing to do when nothing was read since
       the last one */
    const std::string pass_mark =
        std::to_string(batch.id) + ":" + std::to_string(batch.trx_ends.size()) +
        ":" + depends_mark() + (batch.reload ? "r" : "") +
        std::to_string(mview_registry_version.load());
    Apply_result applied = Apply_result::OK;
    if (pinned || refresh_pending || !batch_deferred || pass_mark != partial_mark) {
      const auto apply_start = std::chrono::steady_clock::now();
      applied = apply_batch(!pinned);
      release_pin();
      st_apply_us.fetch_add(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - apply_start)
              .count());
      if (applied == Apply_result::STOP) break;
      partial_mark = applied == Apply_result::OK && batch_deferred
                         ? pass_mark
                         : std::string();
    }
    if (applied == Apply_result::RESTART || applied == Apply_result::REPIN) {
      registry_loaded = registry_loaded &&
                        mview_registry_version.load() == registry_version;
      /* not in a tight loop when the same thing happens again and again */
      restarts++;
      if (applied == Apply_result::RESTART || restarts > 3)
        pause(std::min<unsigned long>(10UL * restarts, 1000UL));
      continue;
    }
    restarts = 0;
    if (batch_deferred) {
      /* until the next pin is allowed, or there is more to read */
      pin_allowed(&pin_wait);
      const unsigned long ms = static_cast<unsigned long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(pin_wait)
              .count());
      wait_deferred(std::min<unsigned long>(ms + (pin_wait.count() ? 1 : 0),
                                            sv_idle_wait_timeout));
    }
  }

  set_state("stopping");
  /* an open transaction is rolled back; the batch is read again */
  for (THD *session : {thd, lock_thd}) session->killed = THD::NOT_KILLED;
  release_pin();
  exec_on(lock_thd, "UNLOCK TABLES", nullptr, nullptr, nullptr);
  exec("ROLLBACK");
  if (state_dirty) save_state();
  reader.reset();
  if (linfo_registered) {
    mysql_bin_log.unregister_log_info(&linfo);
    linfo_registered = false;
  }
  wmv_log(INFORMATION_LEVEL,
          "materialized view daemon stopped: %llu batches, %llu statements, "
          "%llu ms applying, %llu full refreshes, %llu pins (%llu failed, "
          "%llu snapshots), base tables locked %llu ms, waited for %llu ms",
          st_batches.load(), st_statements.load(), st_apply_us.load() / 1000,
          st_full_refreshes.load(), st_pins.load(), st_pin_failures.load(),
          st_snapshots.load(), st_pin_lock_us.load() / 1000,
          st_pin_wait_us.load() / 1000);
  mview_clear_maintained();
  destroy_session();
  set_state("stopped");
}

/* ------------------------------------------------------------------ */
/* Thread and plugin                                                   */
/* ------------------------------------------------------------------ */

static my_thread_handle daemon_thread;
static bool daemon_thread_started = false;

}  // namespace warp_mview

/* Not in the namespace: it is the start routine of the thread. */
extern "C" {
static void *warp_mview_thread(void *) {
  /* must be the first variable: the base of the stack of the session */
  char stack_base = 0;
  my_thread_init();
  {
    warp_mview::Daemon daemon;
    daemon.run(&stack_base);
  }
  my_thread_end();
  return nullptr;
}
}  // extern "C"

int warp_mview_plugin_init(void *) {
  if (init_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs)) return 1;
  warp_mview::stop_requested.store(false);
  warp_mview::set_state("starting");

  my_thread_attr_t attr;
  my_thread_attr_init(&attr);
  my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE);
  /* the stack the server checks statements against */
  if (my_thread_stack_size > 0)
    (void)my_thread_attr_setstacksize(&attr, my_thread_stack_size);
  const int error = my_thread_create(&warp_mview::daemon_thread, &attr,
                                     warp_mview_thread, nullptr);
  my_thread_attr_destroy(&attr);
  if (error != 0) {
    warp_mview::wmv_log(ERROR_LEVEL, "cannot create the daemon thread: %d",
                        error);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }
  warp_mview::daemon_thread_started = true;
  return 0;
}

int warp_mview_plugin_deinit(void *) {
  if (warp_mview::daemon_thread_started) {
    warp_mview::stop_requested.store(true);
    {
      /* end the statement or the wait the thread is in */
      std::lock_guard<std::mutex> guard(warp_mview::session_mutex);
      for (THD *thd : warp_mview::daemon_thds) {
        if (thd == nullptr) continue;
        mysql_mutex_lock(&thd->LOCK_thd_data);
        thd->awake(THD::KILL_CONNECTION);
        mysql_mutex_unlock(&thd->LOCK_thd_data);
      }
    }
    void *result = nullptr;
    my_thread_join(&warp_mview::daemon_thread, &result);
    warp_mview::daemon_thread_started = false;
  }
  deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
  return 0;
}

/* MYSQL_SYSVAR() names the variables without the namespace */
using namespace warp_mview;

struct st_mysql_daemon warp_mview_daemon_descriptor = {
    MYSQL_DAEMON_INTERFACE_VERSION};

SYS_VAR *warp_mview_system_variables[] = {
    MYSQL_SYSVAR(enabled),
    MYSQL_SYSVAR(adopt_refresh),
    MYSQL_SYSVAR(batch_rows),
    MYSQL_SYSVAR(batch_transactions),
    MYSQL_SYSVAR(in_list_chunk_size),
    MYSQL_SYSVAR(full_refresh_threshold),
    MYSQL_SYSVAR(idle_wait_timeout),
    MYSQL_SYSVAR(lock_wait_timeout),
    MYSQL_SYSVAR(retry_limit),
    MYSQL_SYSVAR(pin_lock_wait_timeout),
    MYSQL_SYSVAR(pin_duty_percent),
    MYSQL_SYSVAR(pin_min_interval),
    nullptr};

SHOW_VAR warp_mview_status_variables[] = {
    {"Warp_mview_state", (char *)&warp_mview::show_state, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Warp_mview_binlog_file", (char *)&warp_mview::show_read_file, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Warp_mview_binlog_position", (char *)&warp_mview::show_read_pos,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_applied_binlog_file", (char *)&warp_mview::show_applied_file,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_applied_binlog_position",
     (char *)&warp_mview::show_applied_pos, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_transactions_applied", (char *)&warp_mview::show_transactions,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_batches_applied", (char *)&warp_mview::show_batches,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_rows_applied", (char *)&warp_mview::show_rows, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Warp_mview_keys_applied", (char *)&warp_mview::show_keys, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Warp_mview_apply_microseconds", (char *)&warp_mview::show_apply_us,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_statements", (char *)&warp_mview::show_statements, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Warp_mview_pins", (char *)&warp_mview::show_pins, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Warp_mview_pin_failures", (char *)&warp_mview::show_pin_failures,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_pin_lock_microseconds", (char *)&warp_mview::show_pin_lock_us,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_pin_wait_microseconds", (char *)&warp_mview::show_pin_wait_us,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_pin_snapshots", (char *)&warp_mview::show_snapshots,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_full_refreshes", (char *)&warp_mview::show_full_refreshes,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_apply_errors", (char *)&warp_mview::show_apply_errors,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_maintained_views", (char *)&warp_mview::show_views, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Warp_mview_stale_views", (char *)&warp_mview::show_stale_views,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_stale_view_list", (char *)&warp_mview::show_stale_list,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_mview_lag_seconds", (char *)&warp_mview::show_lag, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

/* The plugin is a library of its own, mview_refresh.so: it does not use the
   WARP storage engine (the views that it maintains can be tables of any
   engine), and a server can have it without ha_warp.so.  Its name, and with
   it the names of its variables (warp_mview_*, Warp_mview_*), is what it
   was when it was the second plugin of ha_warp.so. */
mysql_declare_plugin(mview_refresh){
    MYSQL_DAEMON_PLUGIN,
    &warp_mview_daemon_descriptor,
    "WARP_MVIEW",
    "Justin Swanhart",
    "Maintains table-backed materialized views from the binary log",
    PLUGIN_LICENSE_GPL,
    warp_mview_plugin_init,   /* Plugin Init */
    nullptr,                  /* Plugin check uninstall */
    warp_mview_plugin_deinit, /* Plugin Deinit */
    0x0100,
    warp_mview_status_variables, /* status variables */
    warp_mview_system_variables, /* system variables */
    nullptr,                     /* config options */
    0,                           /* flags */
} mysql_declare_plugin_end;
