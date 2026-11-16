/* Copyright (c) 2018, 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/rapid/rapid_binlog_consumer.cc
  
  RAPID Binlog Consumer - Incremental replication via binlog streaming
  
  This plugin connects to MySQL's binary log as a replication client and
  captures row-level DML changes (INSERT/UPDATE/DELETE) to apply them
  incrementally to DuckDB tables loaded in RAPID.
*/

/* before the headers of the binary log: they include the log service of
   plugins (log_builtins.h), whose macros replace the functions that sql/log.h
   declares unless sql/log.h was included first */
#include "sql/log.h"  // sql_print_error, sql_print_warning

#include "duckdb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "libbinlogevents/include/binlog_event.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "my_sys.h"  // FN_REFLEN
#include "sql/binlog.h"
#include "sql/binlog_reader.h"  // Binlog_file_reader
#include "sql/field.h"
#include "sql/log_event.h"
#include "sql/rpl_gtid.h"
#include "sql/rpl_record.h"     // unpack_row
#include "sql/rpl_replica.h"
#include "sql/rpl_rli.h"        // Relay_log_info
#include "sql/sql_base.h"       // open_table_def, open_table_from_share
#include "sql/sql_class.h"
#include "sql/table.h"
#include "sql/mysqld.h"         // global_system_variables
#include "sql/system_variables.h"
#include "sql/tztime.h"         // Time_zone
#include "sql/log.h"  // sql_print_error, sql_print_warning
#include "decimal.h"  // bin2decimal, decimal2string
#include "my_time.h"  // my_time_packed_from_binary, my_datetime_packed_from_binary

// Forward declare external DuckDB database handle
namespace rapid {
extern duckdb_database db;
}

// Use event type constants from the binlog event library
using namespace mysql::binlog::event;

// Plugin-compatible logging using fprintf to stderr (goes to MySQL error log)
#define LogErr(level, errcode, msg) do { \
  const char *level_str = "INFO"; \
  if (level == ERROR_LEVEL) level_str = "ERROR"; \
  else if (level == WARNING_LEVEL) level_str = "WARNING"; \
  time_t now = time(nullptr); \
  struct tm tm_buf; \
  localtime_r(&now, &tm_buf); \
  char time_str[64]; \
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf); \
  fprintf(stderr, "[%s] [RAPID Binlog] %s: %s\n", time_str, level_str, msg); \
  fflush(stderr); \
} while(0)

// Helper classes to access protected members of Rows_log_event
// Since m_rows_buf, m_curr_row, m_rows_end, m_width are protected, we use inheritance
class Rows_event_accessor : public Rows_log_event {
public:
  const uchar *get_rows_buf_pub() const { return m_rows_buf; }
  const uchar *get_rows_end_pub() const { return m_rows_end; }
  const uchar *get_curr_row_pub() const { return m_curr_row; }
  uint get_width_pub() const { return m_width; }
  TABLE *get_table_pub() const { return m_table; }
};

// Forward declare global mysql_bin_log from the server
// Must be declared outside rapid_binlog namespace to avoid name collision
class MYSQL_BIN_LOG;
extern MYSQL_BIN_LOG mysql_bin_log;

namespace rapid_binlog {

// Configuration
static bool binlog_consumer_enabled = true;
static unsigned long batch_size = 1000;

// Status tracking (current position being read)
static std::atomic<unsigned long> current_binlog_position{4};
static char current_binlog_file[FN_REFLEN] = {0};

// State
static std::atomic<bool> consumer_running{false};
static std::atomic<bool> consumer_initialized{false};
static std::thread *reader_thread = nullptr;
static std::condition_variable reader_cv;
static std::mutex reader_mutex;
static std::mutex init_mutex;

/**
  Track which tables are loaded in RAPID and need incremental updates.
*/
static std::set<std::string> tracked_tables;
static std::mutex tracked_tables_mutex;

/**
  Cache of column names for each table (key: "db.table", value: vector of column names).
  Retrieved from DuckDB schema on first use.
*/
static std::map<std::string, std::vector<std::string>> table_column_names;
static std::mutex column_names_mutex;

/**
  Change record for a single row operation.
*/
struct ChangeRecord {
  enum Type { INSERT, UPDATE, DELETE } type;
  std::string db_name;
  std::string table_name;
  std::vector<std::pair<std::string, std::string>> columns;      // column_name, value
  std::vector<std::pair<std::string, std::string>> old_columns;  // For UPDATE
  
  std::string table_key() const {
    return db_name + "." + table_name;
  }
};

// Queue of changes to apply
static std::queue<ChangeRecord> change_queue;
static std::mutex queue_mutex;

// Column metadata from Table_map event
struct ColumnMetadata {
  std::string name;      // Column name (if available)
  uint8_t type;          // MySQL type code
  uint16_t meta;         // Type-specific metadata
  bool is_nullable;      // Can be NULL
};

// Cache of table definitions from Table_map events
struct TableMapInfo {
  std::string db_name;
  std::string table_name;
  std::vector<ColumnMetadata> columns;  // Column definitions from binlog
  uint column_count;
};

static std::map<uint64_t, TableMapInfo> table_map_cache;
static std::mutex table_map_mutex;

// Forward declaration
static void ensure_binlog_consumer_started();

/**
  Check if a table is tracked for incremental updates.
*/
bool is_table_tracked(const std::string &db_name, const std::string &table_name) {
  std::lock_guard<std::mutex> lock(tracked_tables_mutex);
  std::string key = db_name + "." + table_name;
  return tracked_tables.find(key) != tracked_tables.end();
}

/**
  Register a table for binlog-based incremental updates.
  
  @param db_name Database name
  @param table_name Table name  
  @param table Unused - kept for API compatibility
*/
void register_table_for_binlog(const char *db_name, const char *table_name, TABLE *table [[maybe_unused]]) {
  std::lock_guard<std::mutex> lock(tracked_tables_mutex);
  std::string key = std::string(db_name) + "." + std::string(table_name);
  
  // First registration? Start the binlog consumer thread
  if (tracked_tables.empty()) {
    ensure_binlog_consumer_started();
  }
  
  tracked_tables.insert(key);
}

/**
  Unregister a table from binlog tracking.
*/
void unregister_table_from_binlog(const char *db_name, const char *table_name) {
  std::string key = std::string(db_name) + "." + std::string(table_name);
  
  {
    std::lock_guard<std::mutex> lock(tracked_tables_mutex);
    tracked_tables.erase(key);
  }
  
  // Clear cached column names
  {
    std::lock_guard<std::mutex> lock(column_names_mutex);
    table_column_names.erase(key);
  }
  
  LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
         ("RAPID Binlog: Stopped tracking table " + key).c_str());
}

/**
  Escape a string value for DuckDB SQL.
*/
std::string escape_sql_string(const std::string &value) {
  std::string escaped;
  for (char c : value) {
    if (c == '\'') {
      escaped += "''";
    } else if (c == '\\') {
      escaped += "\\\\";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

/**
  Get column names for a table from DuckDB by querying table metadata.
  Uses "SELECT * FROM table LIMIT 0" to get column names without reading data.
  Results are cached for performance.
  
  @param db_name Database name
  @param table_name Table name
  @param conn DuckDB connection
  @return Vector of column names in order, or empty vector on error
*/
std::vector<std::string> get_column_names_from_duckdb(
    const std::string &db_name,
    const std::string &table_name,
    duckdb_connection conn) {
  
  std::string key = db_name + "." + table_name;

  // Check cache first
  {
    std::lock_guard<std::mutex> lock(column_names_mutex);
    auto it = table_column_names.find(key);
    if (it != table_column_names.end()) {
      return it->second;
    }
  }

  // Query table schema using LIMIT 0 (no data, just metadata)
  std::string query = "SELECT * FROM " + db_name + "." + table_name + " LIMIT 0";
  
  duckdb_result result;
  duckdb_state state = duckdb_query(conn, query.c_str(), &result);
  
  std::vector<std::string> column_names;
  
  if (state == DuckDBSuccess) {
    idx_t column_count = duckdb_column_count(&result);
    
    // Extract column names from result metadata
    for (idx_t i = 0; i < column_count; i++) {
      const char *col_name = duckdb_column_name(&result, i);
      if (col_name) {
        column_names.push_back(std::string(col_name));
      }
    }
    
    duckdb_destroy_result(&result);
    
    // Cache the result
    std::lock_guard<std::mutex> lock(column_names_mutex);
    table_column_names[key] = column_names;
  } else {
    duckdb_destroy_result(&result);
  }
  
  return column_names;
}

/**
  Convert a MySQL Field to SQL value string.
*/
std::string field_to_sql_value(Field *field) {
  if (field->is_null()) {
    return "NULL";
  }
  
  String value_str;
  field->val_str(&value_str);
  
  if (field->str_needs_quotes()) {
    return "'" + escape_sql_string(std::string(value_str.ptr(), value_str.length())) + "'";
  } else {
    return std::string(value_str.ptr(), value_str.length());
  }
}

/**
  Generate INSERT statement for DuckDB.
  
  Note: We don't specify column names, just VALUES. This works as long as
  the columns in the binlog match the table definition order.
*/
std::string generate_insert_sql(const ChangeRecord &change) {
  std::ostringstream sql;
  sql << "INSERT INTO " << change.db_name << "." << change.table_name << " VALUES (";
  
  bool first = true;
  for (const auto &col : change.columns) {
    if (!first) sql << ", ";
    sql << col.second;  // Just the value, not the name
    first = false;
  }
  
  sql << ")";
  return sql.str();
}

/**
  Generate UPDATE statement for DuckDB.
  
  Uses actual column names from DuckDB schema, not placeholder names.
*/
std::string generate_update_sql(const ChangeRecord &change, duckdb_connection conn) {
  std::ostringstream sql;
  sql << "UPDATE " << change.db_name << "." << change.table_name << " SET ";
  
  // Get real column names from DuckDB
  std::vector<std::string> column_names = 
    get_column_names_from_duckdb(change.db_name, change.table_name, conn);
  
  if (column_names.empty()) {
    return "";
  }
  
  // SET clause: Use real column names with new values
  bool first = true;
  for (size_t i = 0; i < change.columns.size() && i < column_names.size(); i++) {
    if (!first) sql << ", ";
    sql << column_names[i] << " = " << change.columns[i].second;
    first = false;
  }
  
  // WHERE clause: Use real column names with old values
  sql << " WHERE ";
  first = true;
  for (size_t i = 0; i < change.old_columns.size() && i < column_names.size(); i++) {
    if (!first) sql << " AND ";
    sql << column_names[i];
    if (change.old_columns[i].second == "NULL") {
      sql << " IS NULL";
    } else {
      sql << " = " << change.old_columns[i].second;
    }
    first = false;
  }
  
  return sql.str();
}

/**
  Generate DELETE statement for DuckDB.
  
  Uses actual column names from DuckDB schema, not placeholder names.
*/
std::string generate_delete_sql(const ChangeRecord &change, duckdb_connection conn) {
  std::ostringstream sql;
  sql << "DELETE FROM " << change.db_name << "." << change.table_name << " WHERE ";
  
  // Get real column names from DuckDB
  std::vector<std::string> column_names = 
    get_column_names_from_duckdb(change.db_name, change.table_name, conn);
  
  if (column_names.empty()) {
    return "";
  }
  
  // WHERE clause: Use real column names with values
  bool first = true;
  for (size_t i = 0; i < change.columns.size() && i < column_names.size(); i++) {
    if (!first) sql << " AND ";
    sql << column_names[i];
    if (change.columns[i].second == "NULL") {
      sql << " IS NULL";
    } else {
      sql << " = " << change.columns[i].second;
    }
    first = false;
  }
  
  return sql.str();
}

/**
  Execute a batch of changes in DuckDB.
*/
bool apply_changes_to_duckdb(std::vector<ChangeRecord> &changes) {
  if (changes.empty()) {
    return false;
  }
  
  duckdb_connection con;
  if (duckdb_connect(rapid::db, &con) == DuckDBError) {
    LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           "RAPID Binlog: Failed to connect to DuckDB");
    return true;
  }
  
  
  duckdb_result result;
  if (duckdb_query(con, "BEGIN TRANSACTION", &result) == DuckDBError) {
    LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           "RAPID Binlog: Failed to begin transaction");
    duckdb_disconnect(&con);
    return true;
  }
  duckdb_destroy_result(&result);
  
  size_t success_count = 0;
  size_t error_count = 0;
  
  // Group INSERTs by table for bulk insert syntax
  // Map: table_name -> vector of VALUES clauses
  std::map<std::string, std::vector<std::string>> insert_batches;
  std::map<std::string, std::string> table_insert_prefix;  // Cache "INSERT INTO table " part
  std::vector<std::string> other_statements;
  
  for (const auto &change : changes) {
    if (change.type == ChangeRecord::INSERT) {
      // Extract the VALUES clause from INSERT INTO table VALUES (...)
      std::string sql = generate_insert_sql(change);
      if (sql.empty()) {
        error_count++;
        continue;
      }
      
      // Build table key for grouping
      std::string table_key = change.db_name + "." + change.table_name;
      
      // Cache the INSERT prefix (only compute once per table)
      if (table_insert_prefix.find(table_key) == table_insert_prefix.end()) {
        size_t values_pos = sql.find("VALUES");
        if (values_pos != std::string::npos) {
          table_insert_prefix[table_key] = sql.substr(0, values_pos); // "INSERT INTO table "
        } else {
          error_count++;
          continue;
        }
      }
      
      // Extract just the VALUES (...) part
      size_t values_pos = sql.find("VALUES");
      if (values_pos != std::string::npos) {
        std::string values_part = sql.substr(values_pos + 7); // Skip "VALUES "
        insert_batches[table_key].push_back(values_part);
      } else {
        error_count++;
      }
    } else {
      // UPDATE and DELETE - execute individually
      std::string sql;
      if (change.type == ChangeRecord::UPDATE) {
        sql = generate_update_sql(change, con);
      } else if (change.type == ChangeRecord::DELETE) {
        sql = generate_delete_sql(change, con);
      }
      
      if (!sql.empty()) {
        other_statements.push_back(sql);
      } else {
        error_count++;
      }
    }
  }
  
  // Execute bulk INSERTs
  for (const auto &batch : insert_batches) {
    const std::string &table_key = batch.first;
    const std::vector<std::string> &values_list = batch.second;
    const std::string &table_prefix = table_insert_prefix[table_key];
    
    // Build: INSERT INTO table VALUES (row1), (row2), (row3), ...
    // Split into chunks of 1000 rows to avoid SQL size limits
    const size_t BULK_INSERT_SIZE = 1000;
    for (size_t i = 0; i < values_list.size(); i += BULK_INSERT_SIZE) {
      std::string sql;
      sql.reserve(65536);  // Reserve 64KB to avoid realloc penalties
      
      sql.append(table_prefix);
      sql.append("VALUES ");
      
      size_t end = std::min(i + BULK_INSERT_SIZE, values_list.size());
      for (size_t j = i; j < end; ++j) {
        if (j > i) sql.append(", ");
        sql.append(values_list[j]);
      }
      
      if (duckdb_query(con, sql.c_str(), &result) == DuckDBError) {
        const char *error = duckdb_result_error(&result);
        LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
               ("RAPID Binlog: Bulk INSERT of " + std::to_string(end - i) + 
                " rows failed. Error: " + (error ? std::string(error) : "unknown")).c_str());
        fprintf(stderr, "[RAPID Binlog] ERROR: Failed SQL: %s\n", sql.c_str());
        error_count += (end - i);
      } else {
        success_count += (end - i);
      }
      duckdb_destroy_result(&result);
    }
  }
  
  // Execute UPDATE and DELETE statements
  for (const auto &sql : other_statements) {
    if (duckdb_query(con, sql.c_str(), &result) == DuckDBError) {
      const char *error = duckdb_result_error(&result);
      LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
             ("RAPID Binlog: Statement failed: " + sql + " Error: " + 
              (error ? std::string(error) : "unknown")).c_str());
      error_count++;
    } else {
      success_count++;
    }
    duckdb_destroy_result(&result);
  }
  
  if (duckdb_query(con, "COMMIT", &result) == DuckDBError) {
    LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           "RAPID Binlog: Failed to commit");
    duckdb_query(con, "ROLLBACK", &result);
    duckdb_destroy_result(&result);
    duckdb_disconnect(&con);
    return true;
  }
  duckdb_destroy_result(&result);
  
  duckdb_disconnect(&con);
  
  if (success_count > 0) {
    LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           ("RAPID Binlog: Applied " + std::to_string(success_count) + 
            " changes" + (error_count > 0 ? " (" + std::to_string(error_count) + 
            " errors)" : "")).c_str());
  }
  
  return error_count > 0;
}

/**
  Flush any pending changes in the queue to DuckDB.
  
  This should be called:
  - When a transaction commits (Xid_log_event or COMMIT statement)
  - On shutdown to ensure no data loss
  - Optionally on SAVEPOINT for fine-grained consistency
*/
void flush_change_queue() {
  std::vector<ChangeRecord> batch;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    while (!change_queue.empty()) {
      batch.push_back(change_queue.front());
      change_queue.pop();
    }
  }
  
  if (!batch.empty()) {
    apply_changes_to_duckdb(batch);
  }
}

/**
  Read a little-endian integer from buffer.
*/
template<typename T>
T read_little_endian(const uchar *&ptr) {
  T value = 0;
  for (size_t i = 0; i < sizeof(T); i++) {
    value |= static_cast<T>(*ptr++) << (i * 8);
  }
  return value;
}

/**
  Parse a single field value from binlog row data.
  
  @param ptr Pointer to field data (will be advanced)
  @param type MySQL field type
  @param meta Type-specific metadata
  @param is_null Whether the field is NULL
  @return SQL string representation of the value
*/
std::string parse_field_value(const uchar *&ptr, uint8_t type, uint16_t meta, bool is_null) {
  if (is_null) {
    return "NULL";
  }
  
  switch (type) {
    case MYSQL_TYPE_TINY: {
      int8_t value = static_cast<int8_t>(*ptr++);
      return std::to_string(value);
    }
    
    case MYSQL_TYPE_SHORT: {
      int16_t value = read_little_endian<int16_t>(ptr);
      return std::to_string(value);
    }
    
    case MYSQL_TYPE_INT24: {
      // MEDIUMINT: 3 bytes
      uint32_t value = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16);
      ptr += 3;
      // Handle sign extension for signed MEDIUMINT
      if (value & 0x800000) {
        value |= 0xFF000000;
      }
      return std::to_string(static_cast<int32_t>(value));
    }
    
    case MYSQL_TYPE_LONG: {
      // INT: 4 bytes
      int32_t value = read_little_endian<int32_t>(ptr);
      return std::to_string(value);
    }
    
    case MYSQL_TYPE_LONGLONG: {
      int64_t value = read_little_endian<int64_t>(ptr);
      return std::to_string(value);
    }
    
    case MYSQL_TYPE_FLOAT: {
      float value;
      memcpy(&value, ptr, sizeof(float));
      ptr += sizeof(float);
      return std::to_string(value);
    }
    
    case MYSQL_TYPE_DOUBLE: {
      double value;
      memcpy(&value, ptr, sizeof(double));
      ptr += sizeof(double);
      return std::to_string(value);
    }
    
    case MYSQL_TYPE_YEAR: {
      uint8_t year = *ptr++;
      return std::to_string(1900 + year);
    }
    
    case MYSQL_TYPE_DATE: {
      // 3 bytes: little-endian integer
      // Bits: 1-5 = day, 6-9 = month, 10-24 = year
      uint32_t date_val = read_little_endian<uint16_t>(ptr);
      date_val |= static_cast<uint32_t>(*ptr++) << 16;
      
      uint day = date_val & 0x1F;
      uint month = (date_val >> 5) & 0x0F;
      uint year = (date_val >> 9);
      
      char buf[32];
      snprintf(buf, sizeof(buf), "'%04u-%02u-%02u'", year, month, day);
      return std::string(buf);
    }
    
    case MYSQL_TYPE_TIME: {
      // 3 bytes: little-endian
      uint32_t time_val = read_little_endian<uint16_t>(ptr);
      time_val |= static_cast<uint32_t>(*ptr++) << 16;
      
      bool negative = (time_val & 0x800000) != 0;
      if (negative) time_val = (~time_val & 0x7FFFFF) + 1;
      
      uint hours = time_val / 10000;
      uint minutes = (time_val / 100) % 100;
      uint seconds = time_val % 100;
      
      char buf[32];
      snprintf(buf, sizeof(buf), "'%s%02u:%02u:%02u'", 
               negative ? "-" : "", hours, minutes, seconds);
      return std::string(buf);
    }
    
    case MYSQL_TYPE_TIME2: {
      // Manual parsing of TIME2 format
      // Format: 3 bytes (big-endian, sign-inverted) + fractional seconds
      uint fsp = meta;
      uint fsp_bytes = (fsp + 1) / 2;
      
      // Read 3 bytes as big-endian, XOR with sign bit
      uint32_t time_val = ((uint32_t)ptr[0] << 16) | ((uint32_t)ptr[1] << 8) | ptr[2];
      ptr += 3;
      
      // Check if negative (sign bit was 0 before XOR)
      bool negative = (time_val & 0x800000) == 0;
      time_val ^= 0x800000;  // Remove sign bit
      
      // Extract time components
      uint hour = (time_val >> 12);
      uint minute = (time_val >> 6) & 0x3F;
      uint second = time_val & 0x3F;
      
      // Read fractional seconds
      uint64_t frac_raw = 0;
      if (fsp > 0) {
        for (uint i = 0; i < fsp_bytes; i++) {
          frac_raw = (frac_raw << 8) | *ptr++;
        }
      }
      
      // Format to string
      char buf[64];
      if (fsp > 0) {
        // Scale fractional part to microseconds
        // The fractional bytes are stored in big-endian and represent
        // the fractional part scaled to use the available byte range
        uint64_t frac_usec;
        switch (fsp) {
          case 1: frac_usec = (frac_raw / 10) * 100000; break;       // 1 byte: value/10 gives deciseconds
          case 2: frac_usec = (frac_raw / 100) * 10000; break;       // 1 byte: value/100 gives centiseconds
          case 3: frac_usec = (frac_raw / 10) * 1000; break;         // 2 bytes: value/10 gives milliseconds
          case 4: frac_usec = (frac_raw / 100) * 100; break;         // 2 bytes: value/100 gives 0.1ms
          case 5: frac_usec = (frac_raw / 100) * 10; break;          // 3 bytes: value/100 gives 10µs
          case 6: frac_usec = frac_raw; break;                       // 3 bytes: value is already in µs
          default: frac_usec = 0; break;
        }
        // Truncate to precision
        uint64_t divisor = 1;
        for (uint i = 0; i < (6 - fsp); i++) divisor *= 10;
        uint64_t frac_display = frac_usec / divisor;
        snprintf(buf, sizeof(buf), "'%s%02u:%02u:%02u.%0*lu'",
                 negative ? "-" : "", hour, minute, second, (int)fsp, (unsigned long)frac_display);
      } else {
        snprintf(buf, sizeof(buf), "'%s%02u:%02u:%02u'",
                 negative ? "-" : "", hour, minute, second);
      }
      return std::string(buf);
    }
    
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME: {
      // Old format: 8 bytes
      uint64_t datetime = read_little_endian<uint64_t>(ptr);
      
      uint second = datetime % 100; datetime /= 100;
      uint minute = datetime % 100; datetime /= 100;
      uint hour = datetime % 100; datetime /= 100;
      uint day = datetime % 100; datetime /= 100;
      uint month = datetime % 100; datetime /= 100;
      uint year = datetime;
      
      char buf[64];
      snprintf(buf, sizeof(buf), "'%04llu-%02llu-%02llu %02llu:%02llu:%02llu'",
               year, month, day, hour, minute, second);
      return std::string(buf);
    }
    
    case MYSQL_TYPE_TIMESTAMP2: {
      // Manual parsing of TIMESTAMP2 format
      // Format: 4 bytes (big-endian Unix timestamp) + fractional seconds
      uint fsp = meta;
      uint fsp_bytes = (fsp + 1) / 2;
      
      // Read 4 bytes as big-endian Unix timestamp
      uint32_t timestamp = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) |
                           ((uint32_t)ptr[2] << 8) | ptr[3];
      ptr += 4;
      
      // Read fractional seconds
      uint64_t frac_raw = 0;
      if (fsp > 0) {
        for (uint i = 0; i < fsp_bytes; i++) {
          frac_raw = (frac_raw << 8) | *ptr++;
        }
      }
      
      /* A TIMESTAMP is a point in time (seconds since 1970 UTC), the column
         of DuckDB is a date and time without a time zone.  It gets the date
         and time of the time zone of the server (@@global.time_zone), as
         ALTER TABLE ... SECONDARY_LOAD stores it (ha_rapid::load_table) and
         as the result of a query is read back (see rapid_server_time_zone
         in ha_rapid.cc).  It was the UTC date and time: rows that came
         through the binary log differed from the rows that were loaded by
         the offset of the time zone. */
      MYSQL_TIME local_time;
      global_system_variables.time_zone->gmt_sec_to_TIME(
          &local_time, static_cast<my_time_t>(timestamp));
      struct tm tm_val;
      memset(&tm_val, 0, sizeof(tm_val));
      tm_val.tm_year = static_cast<int>(local_time.year) - 1900;
      tm_val.tm_mon = static_cast<int>(local_time.month) - 1;
      tm_val.tm_mday = static_cast<int>(local_time.day);
      tm_val.tm_hour = static_cast<int>(local_time.hour);
      tm_val.tm_min = static_cast<int>(local_time.minute);
      tm_val.tm_sec = static_cast<int>(local_time.second);
      
      // Format to string
      char buf[64];
      if (fsp > 0) {
        // Scale fractional part to microseconds
        // The fractional bytes are stored in big-endian and represent
        // the fractional part scaled to use the available byte range
        uint64_t frac_usec;
        switch (fsp) {
          case 1: frac_usec = (frac_raw / 10) * 100000; break;       // 1 byte: value/10 gives deciseconds
          case 2: frac_usec = (frac_raw / 100) * 10000; break;       // 1 byte: value/100 gives centiseconds
          case 3: frac_usec = (frac_raw / 10) * 1000; break;         // 2 bytes: value/10 gives milliseconds
          case 4: frac_usec = (frac_raw / 100) * 100; break;         // 2 bytes: value/100 gives 0.1ms
          case 5: frac_usec = (frac_raw / 100) * 10; break;          // 3 bytes: value/100 gives 10µs
          case 6: frac_usec = frac_raw; break;                       // 3 bytes: value is already in µs
          default: frac_usec = 0; break;
        }
        // Truncate to precision
        uint64_t divisor = 1;
        for (uint i = 0; i < (6 - fsp); i++) divisor *= 10;
        uint64_t frac_display = frac_usec / divisor;
        snprintf(buf, sizeof(buf), "'%04d-%02d-%02d %02d:%02d:%02d.%0*lu'",
                 tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                 tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, (int)fsp, (unsigned long)frac_display);
      } else {
        snprintf(buf, sizeof(buf), "'%04d-%02d-%02d %02d:%02d:%02d'",
                 tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                 tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
      }
      return std::string(buf);
    }
    
    case MYSQL_TYPE_DATETIME2: {
      // Manual parsing of DATETIME2 format
      // Format: 5 bytes (big-endian, packed) + fractional seconds
      uint fsp = meta;
      uint fsp_bytes = (fsp + 1) / 2;
      
      // Read 5 bytes as big-endian 40-bit value
      uint64_t datetime_val = ((uint64_t)ptr[0] << 32) | ((uint64_t)ptr[1] << 24) |
                              ((uint64_t)ptr[2] << 16) | ((uint64_t)ptr[3] << 8) | ptr[4];
      ptr += 5;
      
      // The value is stored with a sign bit, XOR it
      datetime_val ^= 0x8000000000ULL;
      
      // Extract datetime components from 40 bits:
      // 1 bit: sign (not used for valid dates)
      // 17 bits: year*13 + month (year can be 0-9999, month 0-12)
      // 5 bits: day (1-31)
      // 5 bits: hour (0-23)
      // 6 bits: minute (0-59)
      // 6 bits: second (0-59)
      
      uint64_t year_month = (datetime_val >> 22) & 0x1FFFF;
      uint year = year_month / 13;
      uint month = year_month % 13;
      uint day = (datetime_val >> 17) & 0x1F;
      uint hour = (datetime_val >> 12) & 0x1F;
      uint minute = (datetime_val >> 6) & 0x3F;
      uint second = datetime_val & 0x3F;
      
      // Read fractional seconds
      uint64_t frac_raw = 0;
      if (fsp > 0) {
        for (uint i = 0; i < fsp_bytes; i++) {
          frac_raw = (frac_raw << 8) | *ptr++;
        }
      }
      
      // Format to string
      char buf[64];
      if (fsp > 0) {
        // Scale fractional part to microseconds
        // The fractional bytes are stored in big-endian and represent
        // the fractional part scaled to use the available byte range
        uint64_t frac_usec;
        switch (fsp) {
          case 1: frac_usec = (frac_raw / 10) * 100000; break;       // 1 byte: value/10 gives deciseconds
          case 2: frac_usec = (frac_raw / 100) * 10000; break;       // 1 byte: value/100 gives centiseconds
          case 3: frac_usec = (frac_raw / 10) * 1000; break;         // 2 bytes: value/10 gives milliseconds
          case 4: frac_usec = (frac_raw / 100) * 100; break;         // 2 bytes: value/100 gives 0.1ms
          case 5: frac_usec = (frac_raw / 100) * 10; break;          // 3 bytes: value/100 gives 10µs
          case 6: frac_usec = frac_raw; break;                       // 3 bytes: value is already in µs
          default: frac_usec = 0; break;
        }
        // Truncate to precision
        uint64_t divisor = 1;
        for (uint i = 0; i < (6 - fsp); i++) divisor *= 10;
        uint64_t frac_display = frac_usec / divisor;
        snprintf(buf, sizeof(buf), "'%04u-%02u-%02u %02u:%02u:%02u.%0*lu'",
                 year, month, day, hour, minute, second, (int)fsp, (unsigned long)frac_display);
      } else {
        snprintf(buf, sizeof(buf), "'%04u-%02u-%02u %02u:%02u:%02u'",
                 year, month, day, hour, minute, second);
      }
      return std::string(buf);
    }
    
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      // Length-prefixed string
      // Meta contains max length: if < 256, length is 1 byte, else 2 bytes
      uint32_t length;
      if (meta < 256) {
        length = *ptr++;
      } else {
        length = read_little_endian<uint16_t>(ptr);
      }
      
      std::string value(reinterpret_cast<const char*>(ptr), length);
      ptr += length;
      
      // Escape and quote
      return "'" + escape_sql_string(value) + "'";
    }
    
    case MYSQL_TYPE_STRING: {
      // Fixed-length string (CHAR) or ENUM/SET
      // Meta contains max length (high byte) and real type (low byte)
      uint32_t real_type = meta & 0xFF;
      uint32_t max_length = meta >> 8;
      
      if (real_type == MYSQL_TYPE_ENUM || real_type == MYSQL_TYPE_SET) {
        // ENUM/SET stored as integer
        if (max_length == 1) {
          uint8_t val = *ptr++;
          return std::to_string(val);
        } else if (max_length == 2) {
          uint16_t val = read_little_endian<uint16_t>(ptr);
          return std::to_string(val);
        }
      }
      
      // For CHAR with multibyte charset, it's stored with length prefix
      // If max_length < 256, use 1-byte length prefix, else 2-byte
      uint32_t actual_length;
      if (max_length < 256) {
        actual_length = *ptr++;
      } else {
        actual_length = read_little_endian<uint16_t>(ptr);
      }
      
      std::string value(reinterpret_cast<const char*>(ptr), actual_length);
      ptr += actual_length;
      
      // Trim trailing spaces for CHAR
      if (!value.empty()) {
        size_t last = value.find_last_not_of(' ');
        if (last != std::string::npos) {
          value.erase(last + 1);
        } else {
          value.clear(); // All spaces
        }
      }
      
      return "'" + escape_sql_string(value) + "'";
    }
    
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      // Stored as integer (1 or 2 bytes based on count of values)
      if (meta == 1) {
        uint8_t val = *ptr++;
        return std::to_string(val);
      } else if (meta == 2) {
        uint16_t val = read_little_endian<uint16_t>(ptr);
        return std::to_string(val);
      }
      ptr += meta;
      return "1";
    }
    
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: {
      // Length-prefixed BLOB/TEXT
      // Meta determines length bytes: 1, 2, 3, or 4
      uint32_t length = 0;
      for (uint i = 0; i < meta; i++) {
        length |= static_cast<uint32_t>(*ptr++) << (i * 8);
      }
      
      std::string value(reinterpret_cast<const char*>(ptr), length);
      ptr += length;
      
      // If it looks like text (no null bytes), treat as TEXT
      if (value.find('\0') == std::string::npos) {
        return "'" + escape_sql_string(value) + "'";
      } else {
        // Binary data - encode as hex string using DuckDB's blob literal format
        // DuckDB supports: '\xHEXSTRING'::BLOB where HEXSTRING has no separators
        std::string hex = "'\\x";
        hex.reserve(3 + length * 2 + 6);  // '\x' + 2 chars per byte + '::BLOB'
        
        for (size_t i = 0; i < length; i++) {
          char hex_buf[3];
          snprintf(hex_buf, sizeof(hex_buf), "%02x", static_cast<unsigned char>(value[i]));
          hex += hex_buf;
        }
        hex += "'::BLOB";
        return hex;
      }
    }
    
    case MYSQL_TYPE_NEWDECIMAL: {
      // DECIMAL stored in packed binary format
      // Meta: scale (high byte) and precision (low byte)
      uint precision = meta & 0xFF;
      uint scale = meta >> 8;
      
      // Initialize decimal_t structure with buffer
      decimal_digit_t dec_buf[10];  // Sufficient for DECIMAL(65,30)
      decimal_t decimal_value;
      decimal_value.len = 10;
      decimal_value.buf = dec_buf;
      
      // Use MySQL's built-in bin2decimal function to parse the packed BCD format
      int bin_result = bin2decimal(ptr, &decimal_value, precision, scale);
      
      // Calculate how many bytes were consumed
      uint intg = precision - scale;
      uint intg_words = intg / 9;
      uint intg_leftover = intg % 9;
      uint frac_words = scale / 9;
      uint frac_leftover = scale % 9;
      
      static const int dig2bytes[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
      uint bin_size = intg_words * 4 + dig2bytes[intg_leftover] +
                      frac_words * 4 + dig2bytes[frac_leftover];
      
      ptr += bin_size;
      
      if (bin_result != 0) {
        time_t now = time(nullptr);
        struct tm *tm_info = localtime(&now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
        fprintf(stderr, "[%s] [RAPID Binlog] DEBUG: bin2decimal failed with code %d (precision=%u, scale=%u)\n",
                timestamp, bin_result, precision, scale);
        return "NULL";
      }
      
      // Convert decimal to string
      char str_buf[256];  // Sufficient for any DECIMAL value
      int str_len = sizeof(str_buf);
      int result = decimal2string(&decimal_value, str_buf, &str_len, 0, 0);
      
      if (result != 0) {
        time_t now = time(nullptr);
        struct tm *tm_info = localtime(&now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
        fprintf(stderr, "[%s] [RAPID Binlog] DEBUG: decimal2string failed with code %d (precision=%u, scale=%u)\n",
                timestamp, result, precision, scale);
        return "NULL";
      }
      
      // str_buf now contains a null-terminated string
      return std::string(str_buf);
    }
    
    case MYSQL_TYPE_BIT: {
      // BIT stored as (nbits+7)/8 bytes
      // Meta contains bit count
      uint nbits = ((meta >> 8) * 8) + (meta & 0xFF);
      uint nbytes = (nbits + 7) / 8;
      
      // Read as integer
      uint64_t value = 0;
      for (uint i = 0; i < nbytes && i < 8; i++) {
        value |= static_cast<uint64_t>(*ptr++) << (i * 8);
      }
      
      return std::to_string(value);
    }
    
    case MYSQL_TYPE_JSON: {
      // JSON stored as BLOB (length-prefixed binary)
      // Meta determines length bytes
      uint32_t length = 0;
      if (meta == 1) {
        length = *ptr++;
      } else if (meta == 2) {
        length = read_little_endian<uint16_t>(ptr);
      } else if (meta == 3) {
        length = read_little_endian<uint16_t>(ptr);
        length |= static_cast<uint32_t>(*ptr++) << 16;
      } else if (meta == 4) {
        length = read_little_endian<uint32_t>(ptr);
      }
      
      // JSON binary format is complex - for now just extract as string
      std::string value(reinterpret_cast<const char*>(ptr), length);
      ptr += length;
      
      return "'" + escape_sql_string(value) + "'";
    }
    
    case MYSQL_TYPE_GEOMETRY: {
      // GEOMETRY not supported - should have been filtered in process_table_map
      time_t now = time(nullptr);
      struct tm *tm_info = localtime(&now);
      char timestamp[64];
      strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
      sql_print_error("[%s] [RAPID Binlog] [ERROR]: Encountered unsupported GEOMETRY column",
                      timestamp);
      return "NULL";
    }
    
    case MYSQL_TYPE_TYPED_ARRAY:
    case MYSQL_TYPE_INVALID: {
      // Unsupported types - should have been caught in process_table_map
      time_t now_arr = time(nullptr);
      struct tm *tm_info_arr = localtime(&now_arr);
      char timestamp_arr[64];
      strftime(timestamp_arr, sizeof(timestamp_arr), "%Y-%m-%d %H:%M:%S", tm_info_arr);
      sql_print_error("[%s] [RAPID Binlog] [ERROR]: Encountered unsupported column type %d in binlog",
                      timestamp_arr, type);
      return "NULL";
    }
    
    default: {
      // Unknown type - log error and return NULL
      time_t now_unk = time(nullptr);
      struct tm *tm_info_unk = localtime(&now_unk);
      char timestamp_unk[64];
      strftime(timestamp_unk, sizeof(timestamp_unk), "%Y-%m-%d %H:%M:%S", tm_info_unk);
      sql_print_error("[%s] [RAPID Binlog] [ERROR]: Unknown column type %d in binlog",
                      timestamp_unk, type);
      return "NULL";
    }
  }
}

/**
  Parse binlog row data using column metadata from Table_map_event.
  
  @param table_info Table metadata from table_map_cache
  @param row_data Pointer to row data
  @param cols Bitmap of columns present in this image
  @param columns Output: parsed column name/value pairs
*/
const uchar* parse_binlog_row(const TableMapInfo &table_info, const uchar *row_data,
                     const MY_BITMAP *cols,
                     std::vector<std::pair<std::string, std::string>> &columns) {
  columns.clear();
  
  if (!row_data || !cols) {
    return row_data;
  }
  
  // First, skip the NULL bitmap
  // NULL bitmap has one bit per column in the image (not just non-null columns!)
  // Count how many columns are in this row image
  uint columns_in_image = 0;
  for (uint i = 0; i < table_info.column_count; i++) {
    if (bitmap_is_set(cols, i)) {
      columns_in_image++;
    }
  }
  uint null_byte_count = (columns_in_image + 7) / 8;
  const uchar *null_bits = row_data;
  const uchar *field_data = row_data + null_byte_count;
  
  // Iterate through columns
  uint field_index = 0;
  for (uint col_idx = 0; col_idx < table_info.column_count; col_idx++) {
    // Check if this column is in the image
    if (!bitmap_is_set(cols, col_idx)) {
      continue;
    }
    
    if (col_idx >= table_info.columns.size()) {
      break;  // Safety check
    }
    
    const ColumnMetadata &col_meta = table_info.columns[col_idx];
    
    // Check if field is NULL
    uint null_bit_index = field_index;
    uint null_byte = null_bit_index / 8;
    uint null_bit = null_bit_index % 8;
    bool is_null = (null_byte < null_byte_count) && (null_bits[null_byte] & (1 << null_bit));
    
    // Parse the field value (this advances field_data pointer)
    // Parse the field value (this advances field_data pointer)
    std::string value = parse_field_value(field_data, col_meta.type, col_meta.meta, is_null);
    
    columns.push_back({col_meta.name, value});
    field_index++;
  }
  
  // Return pointer after this row (field_data has been advanced by parse_field_value calls)
  return field_data;
}

/**
  Process Write_rows_log_event (INSERT).
*/
void process_write_rows(Write_rows_log_event *event) {
  // Get table info from table map cache
  uint64_t table_id = event->get_table_id().id();
  
  std::lock_guard<std::mutex> lock(table_map_mutex);
  auto it = table_map_cache.find(table_id);
  if (it == table_map_cache.end()) {
    return;  // Table not in cache
  }
  
  const TableMapInfo &table_info = it->second;
  
  if (!is_table_tracked(table_info.db_name, table_info.table_name)) {
    return;  // Not tracking this table
  }
  
  
  // Cast to accessor to get protected fields
  // Use reinterpret_cast since Write_rows_log_event uses virtual inheritance
  Rows_event_accessor *accessor = reinterpret_cast<Rows_event_accessor*>(
    static_cast<Rows_log_event*>(event));
  
  const uchar *rows_buf = accessor->get_rows_buf_pub();
  const uchar *rows_end = accessor->get_rows_end_pub();
  const MY_BITMAP *cols = event->get_cols();
  
  // Parse ALL rows in this event (not just the first one!)
  const uchar *ptr = rows_buf;
  size_t row_count = 0;
  
  while (ptr < rows_end) {
    ChangeRecord change;
    change.type = ChangeRecord::INSERT;
    change.db_name = table_info.db_name;
    change.table_name = table_info.table_name;
    
    // parse_binlog_row returns pointer after this row
    ptr = parse_binlog_row(table_info, ptr, cols, change.columns);
    
    // Queue the change
    {
      std::lock_guard<std::mutex> queue_lock(queue_mutex);
      change_queue.push(change);
    }
    
    row_count++;
  }
}

/**
  Process Update_rows_log_event (UPDATE).
*/
void process_update_rows(Update_rows_log_event *event) {
  uint64_t table_id = event->get_table_id().id();
  
  std::lock_guard<std::mutex> lock(table_map_mutex);
  auto it = table_map_cache.find(table_id);
  if (it == table_map_cache.end()) {
    return;
  }
  
  const TableMapInfo &table_info = it->second;
  
  if (!is_table_tracked(table_info.db_name, table_info.table_name)) {
    return;
  }
  
  
  // Cast to accessor to get protected fields
  Rows_event_accessor *accessor = reinterpret_cast<Rows_event_accessor*>(
    static_cast<Rows_log_event*>(event));
  
  const uchar *rows_buf = accessor->get_rows_buf_pub();
  const uchar *rows_end = accessor->get_rows_end_pub();
  const MY_BITMAP *cols_bi = event->get_cols();      // Before Image columns
  const MY_BITMAP *cols_ai = event->get_cols_ai();   // After Image columns
  
  // Parse ALL rows in this event
  const uchar *ptr = rows_buf;
  size_t row_count = 0;
  
  while (ptr < rows_end) {
    ChangeRecord change;
    change.type = ChangeRecord::UPDATE;
    change.db_name = table_info.db_name;
    change.table_name = table_info.table_name;
    
    // Parse before image (old values)
    ptr = parse_binlog_row(table_info, ptr, cols_bi, change.old_columns);
    
    // Parse after image (new values)
    ptr = parse_binlog_row(table_info, ptr, cols_ai, change.columns);
    
    // Queue the change
    {
      std::lock_guard<std::mutex> queue_lock(queue_mutex);
      change_queue.push(change);
    }
    
    row_count++;
  }
}

/**
  Process Delete_rows_log_event (DELETE).
*/
void process_delete_rows(Delete_rows_log_event *event) {
  uint64_t table_id = event->get_table_id().id();
  
  std::lock_guard<std::mutex> lock(table_map_mutex);
  auto it = table_map_cache.find(table_id);
  if (it == table_map_cache.end()) {
    return;
  }
  
  const TableMapInfo &table_info = it->second;
  
  if (!is_table_tracked(table_info.db_name, table_info.table_name)) {
    return;
  }
  
  
  // Cast to accessor to get protected fields
  Rows_event_accessor *accessor = reinterpret_cast<Rows_event_accessor*>(
    static_cast<Rows_log_event*>(event));
  
  const uchar *rows_buf = accessor->get_rows_buf_pub();
  const uchar *rows_end = accessor->get_rows_end_pub();
  const MY_BITMAP *cols = event->get_cols();
  
  // Parse ALL rows in this event
  const uchar *ptr = rows_buf;
  size_t row_count = 0;
  
  while (ptr < rows_end) {
    ChangeRecord change;
    change.type = ChangeRecord::DELETE;
    change.db_name = table_info.db_name;
    change.table_name = table_info.table_name;
    
    // parse_binlog_row returns pointer after this row
    ptr = parse_binlog_row(table_info, ptr, cols, change.columns);
    
    // Queue the change
    {
      std::lock_guard<std::mutex> queue_lock(queue_mutex);
      change_queue.push(change);
    }
    
    row_count++;
  }
}

/**
  Process Table_map_log_event - cache table metadata.
*/
void process_table_map(Table_map_log_event *event) {
  uint64_t table_id = event->get_table_id().id();
  const char *db_name = event->get_db_name();
  const char *table_name = event->get_table_name();
  
  if (!is_table_tracked(db_name, table_name)) {
    return;  // Don't cache if not tracking
  }
  
  std::lock_guard<std::mutex> lock(table_map_mutex);
  
  // Check if already cached
  if (table_map_cache.find(table_id) != table_map_cache.end()) {
    return;  // Already have this table
  }
  
  TableMapInfo info;
  info.db_name = db_name;
  info.table_name = table_name;
  
  // Extract column metadata from the event
  // Access the base event class to get column information
  mysql::binlog::event::Table_map_event *base_event =
    static_cast<mysql::binlog::event::Table_map_event*>(event);
  
  info.column_count = base_event->m_colcnt;
  
  // Extract column types and metadata
  // Parse m_field_metadata to get metadata for each column
  const unsigned char *meta_ptr = base_event->m_field_metadata;
  for (uint i = 0; i < info.column_count; i++) {
    ColumnMetadata col;
    col.type = base_event->m_coltype[i];
    
    // Validate column type is supported
    if (col.type == MYSQL_TYPE_GEOMETRY || col.type == MYSQL_TYPE_TYPED_ARRAY || col.type == MYSQL_TYPE_INVALID) {
      time_t now = time(nullptr);
      struct tm *tm_info = localtime(&now);
      char timestamp[64];
      strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
      sql_print_error("[%s] [RAPID Binlog] [ERROR]: Table %s.%s contains unsupported column type %d, skipping replication",
                      timestamp, db_name, table_name, col.type);
      return;  // Skip this table
    }
    
    // Extract metadata based on type
    // Different types have different metadata sizes
    switch (col.type) {
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_JSON:
        col.meta = *meta_ptr++;
        break;
      
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIMESTAMP2:
        // 1 byte of metadata (fractional seconds precision 0-6)
        col.meta = meta_ptr[0];
        meta_ptr += 1;
        break;
      
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:
        // 2 bytes of metadata
        col.meta = meta_ptr[0] | (meta_ptr[1] << 8);
        meta_ptr += 2;
        break;
      
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
        // Always 2 bytes of metadata in Table_map_event
        // The metadata value itself indicates how many bytes the actual value uses (1 or 2)
        col.meta = meta_ptr[0] | (meta_ptr[1] << 8);
        meta_ptr += 2;
        break;
      
      default:
        // No metadata or 0 bytes
        col.meta = 0;
        break;
    }
    
    col.is_nullable = false;  // Will check NULL bits if available
    col.name = "col_" + std::to_string(i);  // Default name, may be overridden
    
    // Check if column can be NULL
    if (base_event->m_null_bits) {
      uint byte_pos = i / 8;
      uint bit_pos = i % 8;
      col.is_nullable = (base_event->m_null_bits[byte_pos] & (1 << bit_pos)) != 0;
    }
    
    info.columns.push_back(col);
  }
  
  // Try to extract column names from optional metadata if available
  if (base_event->m_optional_metadata_len > 0 && base_event->m_optional_metadata) {
    // Parse optional metadata for column names
    // Format: type (1 byte) + length + data
    const unsigned char *ptr = base_event->m_optional_metadata;
    const unsigned char *end = ptr + base_event->m_optional_metadata_len;
    
    while (ptr < end) {
      uint8_t field_type = *ptr++;
      if (ptr >= end) break;
      
      // Read field length
      unsigned long field_len = 0;
      if (*ptr < 251) {
        field_len = *ptr++;
      } else {
        // Variable length encoding - for simplicity, skip for now
        break;
      }
      
      if (ptr + field_len > end) break;
      
      // Check if this is COLUMN_NAME metadata
      if (field_type == mysql::binlog::event::Table_map_event::COLUMN_NAME) {
        // Parse column names
        const unsigned char *names_ptr = ptr;
        for (uint i = 0; i < info.column_count && names_ptr < ptr + field_len; i++) {
          // Each name is length-prefixed
          if (names_ptr >= ptr + field_len) break;
          unsigned long name_len = *names_ptr++;
          if (names_ptr + name_len > ptr + field_len) break;
          
          std::string name(reinterpret_cast<const char*>(names_ptr), name_len);
          if (i < info.columns.size()) {
            info.columns[i].name = name;
          }
          names_ptr += name_len;
        }
      }
      
      ptr += field_len;
    }
  }
  
  table_map_cache[table_id] = info;
  
  
  // Debug: print column info
  for (uint i = 0; i < info.columns.size(); i++) {
  }
}

/**
  Process Xid_log_event - transaction commit.
  
  This marks the end of a transaction in ROW format binlog.
  We flush all queued changes to DuckDB to maintain transactional consistency.
*/
void process_xid_event(Xid_log_event *event [[maybe_unused]]) {
  size_t queue_size;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    queue_size = change_queue.size();
  }
  
  if (queue_size > 0) {
    LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           ("RAPID Binlog: XID commit, flushing " + 
            std::to_string(queue_size) + " changes").c_str());
    
    flush_change_queue();
  }
}

/**
  Process Query_log_event - handles COMMIT, ROLLBACK, DDL.
  
  In STATEMENT or MIXED format, COMMIT appears as a Query_log_event.
  We also use this to handle ROLLBACK (discard queue) and DDL statements.
*/
void process_query_event(Query_log_event *event) {
  const char *query = event->query;
  size_t query_len = event->q_len;
  
  if (query == nullptr || query_len == 0) {
    return;
  }
  
  // Check for COMMIT
  if (query_len >= 6 && strncasecmp(query, "COMMIT", 6) == 0) {
    size_t queue_size;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      queue_size = change_queue.size();
    }
    
    if (queue_size > 0) {
      LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
             ("RAPID Binlog: COMMIT statement, flushing " + 
              std::to_string(queue_size) + " changes").c_str());
      flush_change_queue();
    }
  }
  // Check for ROLLBACK
  else if (query_len >= 8 && strncasecmp(query, "ROLLBACK", 8) == 0) {
    // Discard queued changes
    std::lock_guard<std::mutex> lock(queue_mutex);
    size_t discarded = change_queue.size();
    while (!change_queue.empty()) {
      change_queue.pop();
    }
    
    if (discarded > 0) {
      LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
             ("RAPID Binlog: ROLLBACK statement, discarded " + 
              std::to_string(discarded) + " changes").c_str());
    }
  }
  // Check for BEGIN/START TRANSACTION
  else if ((query_len >= 5 && strncasecmp(query, "BEGIN", 5) == 0) ||
           (query_len >= 5 && strncasecmp(query, "START", 5) == 0)) {
    // Transaction start - queue should be empty
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (!change_queue.empty()) {
      LogErr(WARNING_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
             ("RAPID Binlog: Transaction BEGIN but queue has " + 
              std::to_string(change_queue.size()) + " pending changes").c_str());
    }
  }
  // Check for DDL (ALTER TABLE, CREATE, DROP, etc.)
  else if (query_len >= 5 && strncasecmp(query, "ALTER", 5) == 0) {
    // Flush any pending changes before DDL
    flush_change_queue();
    
    LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           ("RAPID Binlog: DDL detected: " + 
            std::string(query, std::min(query_len, size_t(100)))).c_str());
    // In production, you might trigger a full table reload here
  }
  else if (query_len >= 4 && strncasecmp(query, "DROP", 4) == 0) {
    flush_change_queue();
    LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           ("RAPID Binlog: DROP statement: " + 
            std::string(query, std::min(query_len, size_t(100)))).c_str());
  }
}

/**
  Binlog reader thread - reads events from MySQL binlog (Approach A: Local binlog).
  
  This reads events directly from the server's binary log files using
  Binlog_file_reader API.
*/
void binlog_reader_thread_func() {
  fflush(stderr);
  
  // Check if binlog is open
  if (!::mysql_bin_log.is_open()) {
    fprintf(stderr, "[RAPID] ERROR: Binary log is not open - binlog consumer disabled\n");
    fflush(stderr);
    return;
  }
  
  fflush(stderr);
  
  // Determine starting position
  char log_file_name[FN_REFLEN];
  my_off_t start_pos = 4;  // Default binlog position
  
  // Auto-detect: use current active binlog (not the first/oldest one)
  Log_info log_info;
  int result = ::mysql_bin_log.get_current_log(&log_info);
  fflush(stderr);
  
  if (result != 0) {
    fprintf(stderr, "[RAPID] ERROR: Could not get current active binlog (error code: %d)\n", result);
    fflush(stderr);
    return;
  }
  
  if (log_info.log_file_name[0] == '\0') {
    fprintf(stderr, "[RAPID] ERROR: get_current_log returned empty filename\n");
    fflush(stderr);
    return;
  }
  
  strncpy(log_file_name, log_info.log_file_name, FN_REFLEN - 1);
  log_file_name[FN_REFLEN - 1] = '\0';
  
  // Start from current position in the active binlog
  // This ensures we only capture NEW events, not old historical data
  start_pos = log_info.pos;
  
  fflush(stderr);
  
  fflush(stderr);
  
  // Update current position tracking
  strncpy(current_binlog_file, log_file_name, FN_REFLEN - 1);
  current_binlog_file[FN_REFLEN - 1] = '\0';
  current_binlog_position.store(start_pos);
  
  fflush(stderr);
  
  // Create a binlog file reader
  Binlog_file_reader binlog_reader(true /* verify_checksum */);
  
  // Open the binlog file at the starting position
  if (binlog_reader.open(log_file_name, start_pos)) {
    LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           ("RAPID Binlog: Failed to open binlog file: " + 
            std::string(binlog_reader.get_error_str())).c_str());
    return;
  }
  
  // Main event reading loop
  while (consumer_running.load()) {
    if (!binlog_consumer_enabled) {
      // Consumer disabled, sleep and check again
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }
    
    // Read next event from binlog
    Log_event *ev = binlog_reader.read_event_object();
    
    if (ev == nullptr) {
      // Check if we hit EOF or an error
      if (binlog_reader.has_fatal_error()) {
        LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
               ("RAPID Binlog: Fatal error reading binlog: " + 
                std::string(binlog_reader.get_error_str())).c_str());
        break;
      }
      
      // EOF - wait for more events to be written
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }
    
    // Dispatch event based on type
    switch (ev->get_type_code()) {
      case TABLE_MAP_EVENT:
        process_table_map(static_cast<Table_map_log_event*>(ev));
        break;
        
      case WRITE_ROWS_EVENT:
        process_write_rows(static_cast<Write_rows_log_event*>(ev));
        break;
        
      case UPDATE_ROWS_EVENT:
      case PARTIAL_UPDATE_ROWS_EVENT:
        process_update_rows(static_cast<Update_rows_log_event*>(ev));
        break;
        
      case DELETE_ROWS_EVENT:
        process_delete_rows(static_cast<Delete_rows_log_event*>(ev));
        break;
        
      case XID_EVENT:
        process_xid_event(static_cast<Xid_log_event*>(ev));
        break;
        
      case QUERY_EVENT:
        process_query_event(static_cast<Query_log_event*>(ev));
        break;
        
      case ROTATE_EVENT: {
        // Handle binlog rotation - switch to next file
        Rotate_log_event *rotate = static_cast<Rotate_log_event*>(ev);
        
        LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
               ("RAPID Binlog: Rotating to " + std::string(rotate->new_log_ident)).c_str());
        
        // Close current file
        binlog_reader.close();
        
        // Open new file
        strncpy(log_file_name, rotate->new_log_ident, FN_REFLEN - 1);
        log_file_name[FN_REFLEN - 1] = '\0';
        
        if (binlog_reader.open(log_file_name, rotate->pos)) {
          LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
                 ("RAPID Binlog: Failed to open rotated binlog: " + 
                  std::string(binlog_reader.get_error_str())).c_str());
          delete ev;
          break;
        }
        
        // Update tracked position after rotation
        strncpy(current_binlog_file, log_file_name, FN_REFLEN - 1);
        current_binlog_file[FN_REFLEN - 1] = '\0';
        current_binlog_position.store(rotate->pos);
        break;
      }
      
      case FORMAT_DESCRIPTION_EVENT:
        // Format description is handled automatically by the reader
        LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
               "RAPID Binlog: Processed format description event");
        break;
        
      default:
        // Ignore other event types (GTID, heartbeat, etc.)
        break;
    }
    
    // Update current position after processing event
    current_binlog_position.store(binlog_reader.position());
    
    // Clean up the event
    delete ev;
  }
  
  // Clean up
  binlog_reader.close();
  
  LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
         "RAPID Binlog: Reader thread stopped");
}

/**
  Initialize the binlog consumer plugin.
  
  Note: We don't start the binlog reader thread here because the binary log
  may not be open yet. Instead, we defer initialization until the first table
  is registered (see ensure_binlog_consumer_started()).
*/
static int plugin_init(MYSQL_PLUGIN plugin_info [[maybe_unused]]) {
  LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
         "RAPID Binlog Consumer: Ready (will start on first table load)");
  
  return 0;
}

/**
  Start the binlog consumer thread (called lazily on first table registration).
*/
static void ensure_binlog_consumer_started() {
  // Double-checked locking pattern
  if (consumer_initialized.load()) {
    return;  // Already started
  }
  
  std::lock_guard<std::mutex> lock(init_mutex);
  
  // Check again inside the lock
  if (consumer_initialized.load()) {
    return;
  }
  
  if (!binlog_consumer_enabled) {
    LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           "RAPID Binlog Consumer: Disabled via rapid_binlog_enabled");
    return;
  }
  
  consumer_running.store(true);
  
  try {
    reader_thread = new std::thread(binlog_reader_thread_func);
    consumer_initialized.store(true);
    LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           "RAPID Binlog Consumer: Started");
  } catch (const std::exception &e) {
    LogErr(ERROR_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
           ("RAPID Binlog Consumer: Failed to start reader thread: " + 
            std::string(e.what())).c_str());
    consumer_running.store(false);
  }
}

/**
  Deinitialize the binlog consumer plugin.
*/
static int plugin_deinit(MYSQL_PLUGIN plugin_info [[maybe_unused]]) {
  LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
         "RAPID Binlog Consumer: Shutting down");
  
  // Only stop thread if it was started
  if (consumer_initialized.load()) {
    consumer_running.store(false);
    reader_cv.notify_all();
    
    // Wait for reader thread to fully stop before proceeding
    if (reader_thread != nullptr) {
      if (reader_thread->joinable()) {
        reader_thread->join();
      }
      delete reader_thread;
      reader_thread = nullptr;
    }
    
    // Flush any remaining changes to ensure no data loss
    size_t remaining;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      remaining = change_queue.size();
    }
    
    if (remaining > 0) {
      LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
             ("RAPID Binlog Consumer: Flushing " + std::to_string(remaining) + 
              " pending changes before shutdown").c_str());
      flush_change_queue();
    }
    
    // Reset initialization flag AFTER thread is fully stopped
    consumer_initialized.store(false);
  }
  
  // Cleanup
  {
    std::lock_guard<std::mutex> lock(tracked_tables_mutex);
    tracked_tables.clear();
  }
  
  {
    std::lock_guard<std::mutex> lock(table_map_mutex);
    table_map_cache.clear();
  }
  
  LogErr(INFORMATION_LEVEL, ER_SECONDARY_ENGINE_PLUGIN,
         "RAPID Binlog Consumer: Shutdown complete");
  
  return 0;
}

/**
  System variables.
*/
static MYSQL_SYSVAR_BOOL(
    binlog_enabled,
    binlog_consumer_enabled,
    PLUGIN_VAR_RQCMDARG,
    "Enable binlog-based incremental replication to DuckDB",
    nullptr,
    nullptr,
    true);

static MYSQL_SYSVAR_ULONG(
    batch_size,
    batch_size,
    PLUGIN_VAR_RQCMDARG,
    "Number of changes to batch before applying to DuckDB",
    nullptr,
    nullptr,
    1000,
    1,
    100000,
    0);

static SYS_VAR *plugin_system_vars[] = {
    MYSQL_SYSVAR(binlog_enabled),
    MYSQL_SYSVAR(batch_size),
    nullptr};

/**
  Status variables (read-only) showing current binlog reader position.
*/
static int show_current_binlog_file(MYSQL_THD, SHOW_VAR *var, char *buff) {
  var->type = SHOW_CHAR;
  var->value = current_binlog_file;
  return 0;
}

static int show_current_binlog_position(MYSQL_THD, SHOW_VAR *var, char *) {
  var->type = SHOW_LONGLONG;
  var->value = (char *)&current_binlog_position;
  return 0;
}

static SHOW_VAR rapid_binlog_status_vars[] = {
    {"rapid_current_binlog_file",
     (char *)&show_current_binlog_file,
     SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"rapid_current_binlog_position",
     (char *)&show_current_binlog_position,
     SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

}  // namespace rapid_binlog

// Export API - these are called from ha_rapid.cc
// Note: Since this code will be compiled into ha_rapid.so,
// we don't need extern "C" here
void rapid_binlog_register_table(const char *db_name, const char *table_name, TABLE *table) {
  rapid_binlog::register_table_for_binlog(db_name, table_name, table);
}

void rapid_binlog_unregister_table(const char *db_name, const char *table_name) {
  rapid_binlog::unregister_table_from_binlog(db_name, table_name);
}

// Plugin initialization/deinitialization for binlog consumer
// These will be called from ha_rapid's Init/Deinit functions
int rapid_binlog_consumer_init() {
  return rapid_binlog::plugin_init(nullptr);
}

int rapid_binlog_consumer_deinit() {
  return rapid_binlog::plugin_deinit(nullptr);
}

// Export system variables array for registration with main plugin
SYS_VAR **rapid_binlog_get_system_vars() {
  return rapid_binlog::plugin_system_vars;
}

// Export status variables array for registration with main plugin  
SHOW_VAR *rapid_binlog_get_status_vars() {
  return rapid_binlog::rapid_binlog_status_vars;
}
