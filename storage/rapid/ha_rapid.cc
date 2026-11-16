/* Copyright (c) 2025, Justin Swanhart

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 3.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to DuckDB) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 3.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "ha_rapid.h"
#include "storage/rapid/rapid_sql.h"
#include "duckdb.h"
#include "sql/protocol.h"

#include <chrono>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

// External API from rapid_binlog_consumer.cc (compiled into same plugin)
extern void rapid_binlog_register_table(const char *db_name, const char *table_name, TABLE *table);
extern void rapid_binlog_unregister_table(const char *db_name, const char *table_name);
extern int rapid_binlog_consumer_init();
extern int rapid_binlog_consumer_deinit();
extern struct st_mysql_sys_var **rapid_binlog_get_system_vars();
extern struct st_mysql_show_var *rapid_binlog_get_status_vars();

#include <stddef.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "lex_string.h"
#include "my_alloc.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_func.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/opt_trace.h"
#include "sql/query_result.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "sql/sql_executor.h"
#include "sql/sql_select.h"     // count_field_types
#include "sql/sql_tmp_table.h"  // create_tmp_table
#include "sql/temp_table_param.h"
#include "sql/mysqld.h"  // global_system_variables
#include "sql/system_variables.h"
#include "sql/tztime.h"  // Time_zone
#include "template_utils.h"
#include "thr_lock.h"

/* A TIMESTAMP column of MySQL is a point in time, which a session sees in its
   time zone.  The column of DuckDB is a TIMESTAMP without a time zone: it
   holds the date and time of the point in the time zone of the server
   (@@global.time_zone).  The rows that are loaded, the rows that come through
   the binary log and the values that a query returns are all converted with
   it, so a session gets the right point in time whatever its own time zone
   is.  The text of a query is given to DuckDB as it is: a date and time
   constant that is compared with a TIMESTAMP column is taken in the time zone
   of the server, which is the one of the session unless the session changed
   its time zone. */
static inline Time_zone *rapid_server_time_zone() {
  return global_system_variables.time_zone;
}

/* storage/rapid/duckdb/generated_extension_loader_package_build.cpp */
extern "C" int32_t duckdb_register_static_extensions(void);

namespace dd {
class Table;
}

namespace {

// Configuration variable for DuckDB database path
static char *rapid_database_file = nullptr;

static MYSQL_SYSVAR_STR(
    database_file,
    rapid_database_file,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY,
    "Path to DuckDB database file. Use ':memory:' for in-memory database. Read-only, set at startup.",
    nullptr,  // check function
    nullptr,  // update function
    ":memory:"  // default value
);

static MYSQL_THDVAR_BOOL(
    trace_sql, PLUGIN_VAR_OPCMDARG,
    "Give the statement that RAPID runs in DuckDB as a note (SHOW WARNINGS).",
    nullptr, nullptr, false);

static SYS_VAR *rapid_system_variables[] = {
    MYSQL_SYSVAR(database_file),
    MYSQL_SYSVAR(trace_sql),
    nullptr
};

struct RapidShare {
  THR_LOCK lock;
  RapidShare() { thr_lock_init(&lock); }
  ~RapidShare() { thr_lock_delete(&lock); }

  // Not copyable. The THR_LOCK object must stay where it is in memory
  // after it has been initialized.
  RapidShare(const RapidShare &) = delete;
  RapidShare &operator=(const RapidShare &) = delete;
};

// Map from (db_name, table_name) to the RapidShare with table state.
class LoadedTables {
  std::map<std::pair<std::string, std::string>, RapidShare> m_tables;
  std::mutex m_mutex;

 public:
  void add(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables.emplace(std::piecewise_construct, std::make_tuple(db, table),
                     std::make_tuple());
  }

  RapidShare *get(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_tables.find(std::make_pair(db, table));
    return it == m_tables.end() ? nullptr : &it->second;
  }

  void erase(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables.erase(std::make_pair(db, table));
  }
};

LoadedTables *loaded_tables{nullptr};

/**
  Statement context class for the RAPID engine.
*/
class Rapid_statement_context : public Secondary_engine_statement_context {};

/**
  Execution context class for the RAPID engine. It allocates some data
  on the heap when it is constructed, and frees it when it is
  destructed, so that LeakSanitizer and Valgrind can detect if the
  server doesn't destroy the object when the query execution has
  completed.
*/
class Rapid_execution_context : public Secondary_engine_execution_context {
 public:
  Rapid_execution_context() : m_data(std::make_unique<char[]>(10)) {}
  /**
    Checks if the specified cost is the lowest cost seen so far for executing
    the given JOIN.
  */
  bool BestPlanSoFar(const JOIN &join, double cost) {
    if (&join != m_current_join) {
      // No plan has been seen for this join. The current one is best so far.
      m_current_join = &join;
      m_best_cost = cost;
      return true;
    }

    // Check if the current plan is the best seen so far.
    const bool cheaper = cost < m_best_cost;
    m_best_cost = std::min(m_best_cost, cost);
    return cheaper;
  }

  /// Set the DuckDB SQL query to execute
  void SetDuckDBQuery(const std::string &query) { m_duckdb_query = query; }

  /// Get the DuckDB SQL query to execute
  const std::string &GetDuckDBQuery() const { return m_duckdb_query; }

 private:
  std::unique_ptr<char[]> m_data;
  /// The JOIN currently being optimized.
  const JOIN *m_current_join{nullptr};
  /// The cost of the best plan seen so far for the current JOIN.
  double m_best_cost;
  /// The DuckDB SQL query to execute
  std::string m_duckdb_query;
};

}  // namespace

namespace rapid {

ha_rapid::ha_rapid(handlerton *hton, TABLE_SHARE *table_share_arg)
    : handler(hton, table_share_arg) {}

int ha_rapid::open(const char *, int, unsigned int, const dd::Table *) {
  RapidShare *share =
      loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) {
    // The table has not been loaded into the secondary storage engine yet.
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Table has not been loaded");
    return HA_ERR_GENERIC;
  }
  thr_lock_data_init(&share->lock, &m_lock, nullptr);
  return 0;
}

int ha_rapid::info(unsigned int flags) {
  // Get the cardinality statistics from the primary storage engine.
  handler *primary = ha_get_primary_handler();
  int ret = primary->info(flags);
  if (ret == 0) {
    stats.records = primary->stats.records;
  }
  return ret;
}

handler::Table_flags ha_rapid::table_flags() const {
  // Secondary engines do not support index access. Indexes are only used for
  // cost estimates.
  return HA_NO_INDEX_ACCESS;
}

unsigned long ha_rapid::index_flags(unsigned int idx, unsigned int part,
                                   bool all_parts) const {
  const handler *primary = ha_get_primary_handler();
  const unsigned long primary_flags =
      primary == nullptr ? 0 : primary->index_flags(idx, part, all_parts);

  // Inherit the following index flags from the primary handler, if they are
  // set:
  //
  // HA_READ_RANGE - to signal that ranges can be read from the index, so that
  // the optimizer can use the index to estimate the number of rows in a range.
  //
  // HA_KEY_SCAN_NOT_ROR - to signal if the index returns records in rowid
  // order. Used to disable use of the index in the range optimizer if it is not
  // in rowid order.
  return ((HA_READ_RANGE | HA_KEY_SCAN_NOT_ROR) & primary_flags);
}

ha_rows ha_rapid::records_in_range(unsigned int index, key_range *min_key,
                                  key_range *max_key) {
  // Get the number of records in the range from the primary storage engine.
  return ha_get_primary_handler()->records_in_range(index, min_key, max_key);
}

THR_LOCK_DATA **ha_rapid::store_lock(THD *, THR_LOCK_DATA **to,
                                    thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK)
    m_lock.type = lock_type;
  *to++ = &m_lock;
  return to;
}

std::string ha_rapid::escape_string(std::string in) {
  std::string out = "";
  for(size_t i=0;i<in.length();++i) {
    switch(in[i]) {
      /*case '\0':
        out += "\\0";
        break;*/
      case '\'':
        out += "\\'";
        break;
      default:
        out += in[i];
        break;  
    }
  }
  return out;
}

int ha_rapid::create_duckdb_table(const TABLE &table_arg, duckdb_connection con) {
  std::string create_table_query = "";

  // First pass: validate all column types are supported
  for (Field **field = table_arg.field; *field; field++) {
    enum_field_types field_type = (*field)->real_type();
    
    switch(field_type) {
      case MYSQL_TYPE_GEOMETRY:
      case MYSQL_TYPE_TYPED_ARRAY:
      case MYSQL_TYPE_INVALID:
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Unsupported column type in RAPID secondary engine");
        return 1;
      
      default:
        break;
    }
  }

  // Second pass: build CREATE TABLE query
  for (Field **field = table_arg.field; *field; field++) {
    bool is_unsigned = (*field)->all_flags() & UNSIGNED_FLAG;
    bool is_nullable = (*field)->is_nullable();
    
    if(create_table_query != "") {
      create_table_query += ", ";
    }

    create_table_query += std::string((*field)->field_name) + " ";
    switch((*field)->real_type()) {
      case MYSQL_TYPE_TINY:
        create_table_query += "TINYINT";
        break;

      case MYSQL_TYPE_SHORT:
        create_table_query += "SMALLINT";
        break;

      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
        create_table_query += "INTEGER";
        break;

      case MYSQL_TYPE_LONGLONG:
        create_table_query += "BIGINT";
        break;

      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
        create_table_query += "VARCHAR(" + std::to_string((*field)->field_length) + ")";
        break;

      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
        create_table_query += "BLOB";
        break;

      case MYSQL_TYPE_JSON:
        create_table_query += "JSON";
        break;

      case MYSQL_TYPE_FLOAT:
        create_table_query += "REAL";
        break;

      case MYSQL_TYPE_DOUBLE:
        create_table_query += "DOUBLE";
        break;

      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        // Get precision and scale for DECIMAL/NUMERIC types
        uint precision;
        uint8 decimals = (*field)->decimals();
        
        // For NEWDECIMAL, get precision from Field_new_decimal
        if ((*field)->real_type() == MYSQL_TYPE_NEWDECIMAL) {
          Field_new_decimal *dec_field = static_cast<Field_new_decimal *>(*field);
          precision = dec_field->precision;
        } else {
          // For old DECIMAL type
          precision = (*field)->field_length;
        }
        
        create_table_query += "DECIMAL(" + std::to_string(precision) + "," + 
                              std::to_string(decimals) + ")";
        break;
      }

      case MYSQL_TYPE_YEAR:
        create_table_query += "INTEGER";
        break;

      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
        create_table_query += "DATE";
        break;

      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIME2:
        create_table_query += "TIME";
        break;

      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_DATETIME2:
        create_table_query += "TIMESTAMP";
        break;

      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_BOOL:
        create_table_query += "VARCHAR";
        break;
      
      case MYSQL_TYPE_GEOMETRY:
        // GEOMETRY not supported - should have been caught in validation
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Unsupported GEOMETRY type in RAPID secondary engine");
        return 1;
      
      default:
        // Should never reach here due to validation pass
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Unexpected column type in RAPID secondary engine");
        return 1;
    }
    
    if(is_unsigned) {
      create_table_query += " UNSIGNED";
    }
    
    if(!is_nullable) {
      create_table_query += " NOT NULL";
    }
    create_table_query += "\n";
    
  }

  // Detect primary key columns
  std::vector<std::string> pk_columns;
  if (table_arg.s->primary_key != MAX_KEY) {
    // Table has a primary key
    KEY *pk_info = &table_arg.s->key_info[table_arg.s->primary_key];
    for (uint i = 0; i < pk_info->user_defined_key_parts; i++) {
      KEY_PART_INFO *key_part = &pk_info->key_part[i];
      if (key_part->field) {
        pk_columns.push_back(std::string(key_part->field->field_name));
      }
    }
  }

  // Add PRIMARY KEY constraint if we have primary key columns
  std::string pk_constraint = "";
  if (!pk_columns.empty()) {
    pk_constraint = ",\nPRIMARY KEY (";
    for (size_t i = 0; i < pk_columns.size(); i++) {
      if (i > 0) pk_constraint += ", ";
      pk_constraint += pk_columns[i];
    }
    pk_constraint += ")";
  }

  std::string schema_name = std::string(table_arg.s->db.str);
  std::string table_name = std::string(table_arg.s->table_name.str);

  // Create schema if it doesn't exist
  std::string create_schema_query = "CREATE SCHEMA IF NOT EXISTS " + schema_name;
  if (duckdb_query(con, create_schema_query.c_str(), nullptr) == DuckDBError) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Could not create schema in DuckDB");
    return HA_ERR_GENERIC;
  }

  std::string drop_table_query = "DROP TABLE IF EXISTS " +
                                 schema_name + "." + table_name;

  create_table_query = "CREATE TABLE " +
                        schema_name + "." + table_name +
                        "\n(\n" + create_table_query + pk_constraint + "\n)\n";

  if (duckdb_query(con, drop_table_query.c_str(), nullptr) == DuckDBError) {
    // handle error
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Could not drop table in DuckDB");
    return HA_ERR_GENERIC;
  }

  if (duckdb_query(con, create_table_query.c_str(), nullptr) == DuckDBError) {
    // handle error

    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Could not create table in DuckDB");
    return HA_ERR_GENERIC;
  }

  return 0;
}

int ha_rapid::load_table(const TABLE &table_arg,
                        bool *skip_metadata_update [[maybe_unused]]) {
  assert(table_arg.file != nullptr);
  
  loaded_tables->add(table_arg.s->db.str, table_arg.s->table_name.str);
  
  if (loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) ==
      nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_arg.s->db.str,
             table_arg.s->table_name.str);
    return HA_ERR_KEY_NOT_FOUND;
  }

  duckdb_connection con;

  if (duckdb_connect(db, &con) == DuckDBError) {
    // handle error
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Could not connect to DuckDB database");    
    return HA_ERR_GENERIC;
  }

  if(create_duckdb_table(table_arg, con)) {
    return HA_ERR_GENERIC;
  }

  auto res = table_arg.file->ha_rnd_init(true);

  std::string schema_name = std::string(table_arg.s->db.str);
  std::string table_name = std::string(table_arg.s->table_name.str);

  // Create appender for fast bulk inserts (much faster than SQL strings)
  duckdb_appender appender;
  if (duckdb_appender_create(con, schema_name.c_str(), table_name.c_str(), &appender) == DuckDBError) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Could not create DuckDB appender");
    duckdb_disconnect(&con);
    return HA_ERR_GENERIC;
  }
  
  size_t rows_loaded = 0;
  const size_t FLUSH_INTERVAL = 10000;  // Flush every N rows for progress feedback
  
  // Read rows from MySQL and append directly to DuckDB
  while (table_arg.file->ha_rnd_next(table_arg.record[0]) == 0) {
    // Begin a new row in the appender
    if (duckdb_appender_begin_row(appender) == DuckDBError) {
      const char *err = duckdb_appender_error(appender);
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err ? err : "Appender begin_row failed");
      duckdb_appender_destroy(&appender);
      duckdb_disconnect(&con);
      return HA_ERR_GENERIC;
    }
    
    // Append each field value
    for (Field **field = table_arg.field; *field; field++) {
      if ((*field)->is_null()) {
        duckdb_append_null(appender);
        continue;
      }
      
      // Append based on field type
      switch ((*field)->type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
          duckdb_append_int32(appender, (*field)->val_int());
          break;
          
        case MYSQL_TYPE_LONGLONG:
          duckdb_append_int64(appender, (*field)->val_int());
          break;
          
        case MYSQL_TYPE_FLOAT:
          duckdb_append_float(appender, (*field)->val_real());
          break;
          
        case MYSQL_TYPE_DOUBLE:
          duckdb_append_double(appender, (*field)->val_real());
          break;
          
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_JSON: {
          String tmp;
          auto s = (*field)->val_str(&tmp);
          duckdb_append_varchar_length(appender, s->ptr(), s->length());
          break;
        }
        
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          String tmp;
          auto s = (*field)->val_str(&tmp);
          // Use duckdb_append_blob for proper binary data handling
          duckdb_append_blob(appender, s->ptr(), s->length());
          break;
        }
        
        case MYSQL_TYPE_GEOMETRY:
          // GEOMETRY not supported
          duckdb_append_null(appender);
          break;

        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2: {
          /* the date and time of the time zone of the server, not of the
             session that loads the table: see rapid_server_time_zone */
          my_timeval tv;
          int warnings = 0;
          MYSQL_TIME lt;
          if ((*field)->get_timestamp(&tv, &warnings)) {
            duckdb_append_null(appender);
            break;
          }
          rapid_server_time_zone()->gmt_sec_to_TIME(&lt, tv);
          char buf[40];
          const int len = snprintf(buf, sizeof(buf),
                                   "%04u-%02u-%02u %02u:%02u:%02u.%06lu",
                                   lt.year, lt.month, lt.day, lt.hour,
                                   lt.minute, lt.second,
                                   static_cast<unsigned long>(tv.m_tv_usec));
          duckdb_append_varchar_length(appender, buf, len);
          break;
        }
        
        default: {
          // For unsupported types, convert to string
          String tmp;
          auto s = (*field)->val_str(&tmp);
          duckdb_append_varchar_length(appender, s->ptr(), s->length());
          break;
        }
      }
    }
    
    // End the row
    if (duckdb_appender_end_row(appender) == DuckDBError) {
      const char *err = duckdb_appender_error(appender);
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err ? err : "Appender end_row failed");
      duckdb_appender_destroy(&appender);
      duckdb_disconnect(&con);
      return HA_ERR_GENERIC;
    }
    
    rows_loaded++;
    
    // Flush periodically for better memory management
    if (rows_loaded % FLUSH_INTERVAL == 0) {
      duckdb_appender_flush(appender);
    }
  }
  
  // Final flush and close the appender
  if (duckdb_appender_close(appender) == DuckDBError) {
    const char *err = duckdb_appender_error(appender);
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err ? err : "Appender close failed");
    duckdb_appender_destroy(&appender);
    duckdb_disconnect(&con);
    return HA_ERR_GENERIC;
  }
  
  duckdb_appender_destroy(&appender);
  
  table_arg.file->ha_rnd_end();
  duckdb_disconnect(&con);
  
  // Register the table with the binlog consumer for incremental DML replication
  // We cast away const because the binlog consumer needs to use the TABLE for unpack_row
  rapid_binlog_register_table(table_arg.s->db.str, table_arg.s->table_name.str, 
                               const_cast<TABLE*>(&table_arg));
  
  return 0;
}

int ha_rapid::unload_table(const char *db_name, const char *table_name,
                          bool error_if_not_loaded) {
  if (error_if_not_loaded &&
      loaded_tables->get(db_name, table_name) == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Table is not loaded on a secondary engine");
    return 1;
  } else {
    
    duckdb_connection con;

    if (duckdb_connect(db, &con) == DuckDBError) {
      // handle error
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Could not connect to DuckDB database");
    }
    std::string drop_table_query = "DROP TABLE IF EXISTS " + std::string(db_name) + "." + std::string(table_name);
    if (duckdb_query(con, drop_table_query.c_str(), nullptr) == DuckDBError) {
      // handle error
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Could not drop table in DuckDB");
    }
    // cleanup
    duckdb_disconnect(&con);
    
  }
  loaded_tables->erase(db_name, table_name);
  
  // Unregister from binlog consumer
  rapid_binlog_unregister_table(db_name, table_name);
  
  return 0;
}

/**
  Convert a LEX* into the SQL string that originated the AST.
  
  This function retrieves the original SQL query string that was parsed to create
  the LEX AST. The original query is stored in the THD (Thread Descriptor) object
  that is associated with the LEX.
  
  @param lex The LEX object containing the parsed AST
  @return The original SQL query string as a LEX_CSTRING, or NULL_CSTR if invalid
  
  @note The returned string is the original query as entered by the user,
        before any rewriting or normalization. For rewritten queries, use
        thd->rewritten_query() instead.
        
  @example
    LEX *lex = thd->lex;
    LEX_CSTRING original_sql = lex_to_sql_string(lex);
    if (original_sql.str != nullptr) {
      printf("Original SQL: %.*s\n", (int)original_sql.length, original_sql.str);
    }
*/
LEX_CSTRING lex_to_sql_string(LEX *lex) {
  if (lex == nullptr || lex->thd == nullptr) {
    return NULL_CSTR;
  }
  
  // The original SQL query string is stored in THD::m_query_string
  // and can be accessed via thd->query()
  return lex->thd->query();
}

}  // namespace rapid

namespace {
bool SecondaryEnginePrePrepareHook(THD *thd) {
  // Don't offload INSERT/UPDATE/DELETE statements to secondary engine
  // These should always execute on the primary engine (InnoDB)
  LEX *lex = thd->lex;
  if (lex->sql_command == SQLCOM_INSERT || 
      lex->sql_command == SQLCOM_INSERT_SELECT ||
      lex->sql_command == SQLCOM_UPDATE ||
      lex->sql_command == SQLCOM_UPDATE_MULTI ||
      lex->sql_command == SQLCOM_DELETE ||
      lex->sql_command == SQLCOM_DELETE_MULTI ||
      lex->sql_command == SQLCOM_REPLACE ||
      lex->sql_command == SQLCOM_REPLACE_SELECT ||
      lex->sql_command == SQLCOM_LOAD) {
    return false;  // Don't use secondary engine
  }

  if (thd->m_current_query_cost <=
      static_cast<double>(thd->variables.secondary_engine_cost_threshold)) {
    Opt_trace_context *const trace = &thd->opt_trace;
    if (trace->is_started()) {
      const Opt_trace_object wrapper(trace);
      Opt_trace_object oto(trace, "secondary_engine_not_used");
      oto.add_alnum("reason",
                    "The estimated query cost does not exceed "
                    "secondary_engine_cost_threshold.");
      oto.add("cost", thd->m_current_query_cost);
      oto.add("threshold", thd->variables.secondary_engine_cost_threshold);
    }
    return false;
  }

  if (thd->secondary_engine_statement_context() == nullptr) {
    /* Prepare this query's specific statment context */
    std::unique_ptr<Secondary_engine_statement_context> ctx =
        std::make_unique<Rapid_statement_context>();
    thd->set_secondary_engine_statement_context(std::move(ctx));
  }
  return true;
}
}  // namespace

static bool PrepareSecondaryEngine(THD *thd, LEX *lex) {
  DBUG_EXECUTE_IF("secondary_engine_rapid_prepare_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  // Don't offload INSERT/UPDATE/DELETE statements to secondary engine
  // These should always execute on the primary engine (InnoDB)
  if (lex->sql_command == SQLCOM_INSERT || 
      lex->sql_command == SQLCOM_INSERT_SELECT ||
      lex->sql_command == SQLCOM_UPDATE ||
      lex->sql_command == SQLCOM_UPDATE_MULTI ||
      lex->sql_command == SQLCOM_DELETE ||
      lex->sql_command == SQLCOM_DELETE_MULTI ||
      lex->sql_command == SQLCOM_REPLACE ||
      lex->sql_command == SQLCOM_REPLACE_SELECT ||
      lex->sql_command == SQLCOM_LOAD) {
    return true;  // Reject offloading
  }

  auto context = new (thd->mem_root) Rapid_execution_context;
  if (context == nullptr) return true;
  lex->set_secondary_engine_execution_context(context);

  // Disable use of constant tables and evaluation of subqueries during
  // optimization.
  lex->add_statement_options(OPTION_NO_CONST_TABLES |
                             OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);

  return false;
}

static void AssertSupportedPath(const AccessPath *path) {
  switch (path->type) {
    // The only supported join type is hash join. Other join types are disabled
    // in handlerton::secondary_engine_flags.
    case AccessPath::NESTED_LOOP_JOIN: /* purecov: deadcode */
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
    case AccessPath::BKA_JOIN:
    // Index access is disabled in ha_rapid::table_flags(), so we should see none
    // of these access types.
    case AccessPath::INDEX_SCAN:
    case AccessPath::INDEX_DISTANCE_SCAN: /* purecov: deadcode */
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::EQ_REF:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::INDEX_RANGE_SCAN:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
    case AccessPath::ROWID_INTERSECTION:
    case AccessPath::ROWID_UNION:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      assert(false); /* purecov: deadcode */
      break;
    default:
      break;
  }

  // This secondary storage engine does not yet store anything in the auxiliary
  // data member of AccessPath.
  assert(path->secondary_engine_data == nullptr);
}

/**
  Convert a DuckDB value to a MySQL field.
  
  @param result      The DuckDB query result
  @param col         The column index
  @param row         The row index  
  @param field       The MySQL field to store the value in
  @return true on error, false on success
*/
static bool ConvertDuckDBValueToField(duckdb_result *result, idx_t col, idx_t row,
                                      Field *field,
                                      bool timestamp_column = false) {
  // Check if value is NULL
  if (duckdb_value_is_null(result, col, row)) {
    field->set_null();
    return false;
  }

  field->set_notnull();
  
  // Get the column type
  duckdb_type type = duckdb_column_type(result, col);
  
  switch (type) {
    case DUCKDB_TYPE_BOOLEAN: {
      bool val = duckdb_value_boolean(result, col, row);
      field->store(val ? 1 : 0, false);
      break;
    }
    case DUCKDB_TYPE_TINYINT: {
      int8_t val = duckdb_value_int8(result, col, row);
      field->store(val, false);
      break;
    }
    case DUCKDB_TYPE_SMALLINT: {
      int16_t val = duckdb_value_int16(result, col, row);
      field->store(val, false);
      break;
    }
    case DUCKDB_TYPE_INTEGER: {
      int32_t val = duckdb_value_int32(result, col, row);
      field->store(val, false);
      break;
    }
    case DUCKDB_TYPE_BIGINT: {
      int64_t val = duckdb_value_int64(result, col, row);
      field->store(val, false);
      break;
    }
    case DUCKDB_TYPE_UTINYINT: {
      uint8_t val = duckdb_value_uint8(result, col, row);
      field->store(val, true);
      break;
    }
    case DUCKDB_TYPE_USMALLINT: {
      uint16_t val = duckdb_value_uint16(result, col, row);
      field->store(val, true);
      break;
    }
    case DUCKDB_TYPE_UINTEGER: {
      uint32_t val = duckdb_value_uint32(result, col, row);
      field->store(val, true);
      break;
    }
    case DUCKDB_TYPE_UBIGINT: {
      uint64_t val = duckdb_value_uint64(result, col, row);
      field->store(val, true);
      break;
    }
    case DUCKDB_TYPE_FLOAT: {
      float val = duckdb_value_float(result, col, row);
      field->store(val);
      break;
    }
    case DUCKDB_TYPE_DOUBLE: {
      double val = duckdb_value_double(result, col, row);
      field->store(val);
      break;
    }
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_BLOB: {
      char *val = duckdb_value_varchar(result, col, row);
      if (val != nullptr) {
        field->store(val, strlen(val), &my_charset_utf8mb4_bin);
        duckdb_free(val);
      }
      break;
    }
    case DUCKDB_TYPE_DATE: {
      duckdb_date date = duckdb_value_date(result, col, row);
      duckdb_date_struct date_struct = duckdb_from_date(date);
      /* a DATE is stored as a Date_val: Field::store_time() of this server
         does not take a MYSQL_TIME that is a date */
      field->store_date(Date_val(static_cast<uint32_t>(date_struct.year),
                                 static_cast<uint32_t>(date_struct.month),
                                 static_cast<uint32_t>(date_struct.day)));
      break;
    }
    case DUCKDB_TYPE_TIME: {
      duckdb_time time = duckdb_value_time(result, col, row);
      duckdb_time_struct time_struct = duckdb_from_time(time);
      MYSQL_TIME mysql_time;
      memset(&mysql_time, 0, sizeof(mysql_time));
      mysql_time.hour = time_struct.hour;
      mysql_time.minute = time_struct.min;
      mysql_time.second = time_struct.sec;
      mysql_time.second_part = time_struct.micros;
      mysql_time.time_type = MYSQL_TIMESTAMP_TIME;
      /* as text, with the fraction: the field keeps as many digits as it
         has */
      char time_text[32];
      const int time_length =
          snprintf(time_text, sizeof(time_text), "%02u:%02u:%02u.%06lu",
                   mysql_time.hour, mysql_time.minute, mysql_time.second,
                   static_cast<unsigned long>(mysql_time.second_part));
      field->store(time_text, time_length, &my_charset_utf8mb4_bin);
      break;
    }
    case DUCKDB_TYPE_TIMESTAMP: {
      duckdb_timestamp timestamp = duckdb_value_timestamp(result, col, row);
      duckdb_timestamp_struct ts_struct = duckdb_from_timestamp(timestamp);
      MYSQL_TIME mysql_time;
      memset(&mysql_time, 0, sizeof(mysql_time));
      mysql_time.year = ts_struct.date.year;
      mysql_time.month = ts_struct.date.month;
      mysql_time.day = ts_struct.date.day;
      mysql_time.hour = ts_struct.time.hour;
      mysql_time.minute = ts_struct.time.min;
      mysql_time.second = ts_struct.time.sec;
      mysql_time.second_part = ts_struct.time.micros;
      mysql_time.time_type = MYSQL_TIMESTAMP_DATETIME;
      if (field->type() == MYSQL_TYPE_TIMESTAMP || timestamp_column) {
        /* a point in time: the date and time of the time zone of the server
           (see rapid_server_time_zone), which a session sees in its own
           time zone */
        bool in_dst_time_gap = false;
        my_timeval tv;
        tv.m_tv_sec = rapid_server_time_zone()->TIME_to_gmt_sec(
            &mysql_time, &in_dst_time_gap);
        tv.m_tv_usec = ts_struct.time.micros;
        if (field->type() == MYSQL_TYPE_TIMESTAMP) {
          field->store_timestamp(&tv);
        } else {
          /* the field of the result is not a TIMESTAMP: the date and time
             of the time zone of the session */
          MYSQL_TIME local_time;
          current_thd->time_zone()->gmt_sec_to_TIME(&local_time, tv);
          local_time.second_part = ts_struct.time.micros;
          field->store_time(&local_time, 6);
        }
      } else {
        field->store_time(&mysql_time, 6);
      }
      break;
    }
    default: {
      // For unsupported types, convert to string
      char *val = duckdb_value_varchar(result, col, row);
      if (val != nullptr) {
        field->store(val, strlen(val), &my_charset_utf8mb4_bin);
        duckdb_free(val);
      }
      break;
    }
  }
  
  return false;
}

/**
  Execute a DuckDB query and stream results back to MySQL.
  
  @param join         JOIN object
  @param query_result Query result object for sending rows
  @return true on error, false on success
*/
static bool ExecuteDuckDBQuery(JOIN *join, Query_result *query_result) {
  THD *thd = join->thd;
  LEX *lex = thd->lex;

  /* The server calls this for every query block of the statement (the
     blocks of a UNION): the statement of DuckDB is the whole statement, it
     is run for the first one. */
  if (join->query_block !=
      join->query_block->master_query_expression()->first_query_block()) {
    return false;
  }
  
  // Get the DuckDB query from the execution context
  auto *context = down_cast<Rapid_execution_context *>(
      lex->secondary_engine_execution_context());
  const std::string &duckdb_sql = context->GetDuckDBQuery();
  
  if (duckdb_sql.empty()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "No DuckDB query found in execution context");
    return true;
  }
  
  // Connect to DuckDB
  duckdb_connection con;
  if (duckdb_connect(rapid::db, &con) == DuckDBError) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Could not connect to DuckDB database");
    return true;
  }
  
  /* Execute the query.  duckdb_query() only returns when the query has
     ended: a thread watches the session meanwhile and interrupts the query
     in DuckDB when the statement is killed (KILL, max_execution_time, the
     end of the server). */
  duckdb_result result;
  memset(&result, 0, sizeof(result));
  {
    std::atomic<bool> finished{false};
    std::atomic<bool> interrupted{false};
    std::thread watcher([&finished, &interrupted, thd, con]() {
      while (!finished.load()) {
        if (thd->killed != THD::NOT_KILLED) {
          interrupted.store(true);
          duckdb_interrupt(con);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
    const duckdb_state state = duckdb_query(con, duckdb_sql.c_str(), &result);
    finished.store(true);
    watcher.join();
    if (state == DuckDBError) {
      if (interrupted.load() && thd->killed != THD::NOT_KILLED) {
        thd->send_kill_message();
      } else {
        const char *error = duckdb_result_error(&result);
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 error ? error : "DuckDB query execution failed");
      }
      duckdb_destroy_result(&result);
      duckdb_disconnect(&con);
      return true;
    }
  }

  // Get the result set metadata
  idx_t row_count = duckdb_row_count(&result);
  idx_t column_count = duckdb_column_count(&result);

  /* The rows are returned through a table that has the columns of the result
     of the statement, with the types that MySQL gave them: a value of
     DuckDB is stored in its field (which converts and formats it as MySQL
     would) and the row of fields is given to the receiver of the result
     (the client, or the table of INSERT ... SELECT and CREATE TABLE ...
     SELECT).  The table is only its row buffer, it is never opened. */
  Query_expression *unit = join->query_block->master_query_expression();
  mem_root_deque<Item *> holders(thd->mem_root);
  std::vector<bool> timestamp_columns;
  for (Item *item : VisibleFields(*unit->get_unit_column_types())) {
    timestamp_columns.push_back(item->data_type() == MYSQL_TYPE_TIMESTAMP ||
                                item->data_type() == MYSQL_TYPE_TIMESTAMP2);
    Item_type_holder *holder = new (thd->mem_root) Item_type_holder(thd, item);
    /* as for the columns of a UNION: the type of the column is the type of
       its (one) expression, with its precision */
    if (holder == nullptr || holder->unify_types(item)) {
      duckdb_destroy_result(&result);
      duckdb_disconnect(&con);
      return true;
    }
    holders.push_back(holder);
  }
  if (holders.size() != column_count) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "The result of DuckDB does not have the columns of the statement");
    duckdb_destroy_result(&result);
    duckdb_disconnect(&con);
    return true;
  }
  Temp_table_param tmp_table_param;
  count_field_types(join->query_block, &tmp_table_param, holders, false, true);
  tmp_table_param.skip_create_table = true;
  TABLE *row_table =
      create_tmp_table(thd, &tmp_table_param, holders, nullptr, false, true,
                       TMP_TABLE_ALL_COLUMNS, HA_POS_ERROR, "rapid_result");
  if (row_table == nullptr) {
    duckdb_destroy_result(&result);
    duckdb_disconnect(&con);
    return true;
  }
  mem_root_deque<Item *> row_items(thd->mem_root);
  bool error = false;
  for (uint i = 0; i < row_table->s->fields && !error; i++) {
    Item *item = new (thd->mem_root) Item_field(row_table->field[i]);
    error = item == nullptr;
    if (!error) row_items.push_back(item);
  }
  error = error || row_items.size() != column_count;

  for (idx_t row = 0; row < row_count && !error; ++row) {
    for (idx_t col = 0; col < column_count && !error; ++col) {
      Field *field = row_table->field[col];
      if (duckdb_value_is_null(&result, col, row)) {
        /* a column that MySQL takes for NOT NULL has its zero value */
        if (field->is_nullable())
          field->set_null();
        else
          field->reset();
        continue;
      }
      error = ConvertDuckDBValueToField(&result, col, row, field,
                                        timestamp_columns[col]);
    }
    if (error || thd->killed) {
      error = true;
      break;
    }
    if (query_result->send_data(thd, row_items)) {
      error = true;
      break;
    }
    thd->inc_sent_row_count(1);
    join->send_records++;
  }

  close_tmp_table(row_table);
  free_tmp_table(row_table);
  duckdb_destroy_result(&result);
  duckdb_disconnect(&con);

  return error;
}

/* handlerton::secondary_engine_resolved: the statement for DuckDB is written
   from the query as it was resolved, before the optimizer changes its
   conditions in place. */
static bool SecondaryEngineResolved(THD *thd, LEX *lex) {
  auto *context = down_cast<Rapid_execution_context *>(
      lex->secondary_engine_execution_context());
  if (context == nullptr) return false;
  {
    std::string duckdb_sql, why;
    if (rapid::generate_duckdb_sql(thd, lex, &duckdb_sql, &why)) {
      const std::string message =
          "The statement can not be run by RAPID: it has " + why;
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), message.c_str());
      return true;
    }
    context->SetDuckDBQuery(duckdb_sql);
    if (THDVAR(thd, trace_sql)) {
      /* a note holds about 500 bytes: the statement in parts */
      const size_t part = 400;
      for (size_t at = 0, n = 1; at < duckdb_sql.length(); at += part, n++) {
        push_warning_printf(thd, Sql_condition::SL_NOTE,
                            ER_SECONDARY_ENGINE_PLUGIN, "RAPID %zu: %s", n,
                            duckdb_sql.substr(at, part).c_str());
      }
    }
  }
  return false;
}

static bool OptimizeSecondaryEngine(THD *thd [[maybe_unused]], LEX *lex) {
  // The context should have been set by PrepareSecondaryEngine.
  assert(lex->secondary_engine_execution_context() != nullptr);

  /* the statement for DuckDB was written by PrepareSecondaryEngine */
  auto *context = down_cast<Rapid_execution_context *>(
      lex->secondary_engine_execution_context());
  if (!context->GetDuckDBQuery().empty()) {
    // Set the external executor on all query blocks' JOIN objects
    for (Query_block *select = lex->unit->first_query_block(); select != nullptr;
         select = select->next_query_block()) {
      if (select->join != nullptr) {
        select->join->override_executor_func = ExecuteDuckDBQuery;
      }
    }
  }

  DBUG_EXECUTE_IF("secondary_engine_rapid_optimize_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  DEBUG_SYNC(thd, "before_rapid_optimize");

  if (lex->using_hypergraph_optimizer()) {
    WalkAccessPaths(lex->unit->root_access_path(), nullptr,
                    WalkAccessPathPolicy::ENTIRE_TREE,
                    [](AccessPath *path, const JOIN *) {
                      AssertSupportedPath(path);
                      return false;
                    });
  }

  return false;
}

static bool CompareJoinCost(THD *thd, const JOIN &join, double optimizer_cost,
                            bool *use_best_so_far, bool *cheaper,
                            double *secondary_engine_cost) {
  *use_best_so_far = false;

  DBUG_EXECUTE_IF("secondary_engine_rapid_compare_cost_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  DBUG_EXECUTE_IF("secondary_engine_rapid_choose_first_plan", {
    *use_best_so_far = true;
    *cheaper = true;
    *secondary_engine_cost = optimizer_cost;
  });

  // Just use the cost calculated by the optimizer by default.
  *secondary_engine_cost = optimizer_cost;

  // This debug flag makes the cost function prefer orders where a table with
  // the alias "X" is closer to the beginning.
  DBUG_EXECUTE_IF("secondary_engine_rapid_change_join_order", {
    double cost = join.tables;
    for (size_t i = 0; i < join.tables; ++i) {
      const Table_ref *ref = join.positions[i].table->table_ref;
      if (std::string(ref->alias) == "X") {
        cost += i;
      }
    }
    *secondary_engine_cost = cost;
  });

  // Check if the calculated cost is cheaper than the best cost seen so far.
  *cheaper = down_cast<Rapid_execution_context *>(
                 thd->lex->secondary_engine_execution_context())
                 ->BestPlanSoFar(join, *secondary_engine_cost);

  return false;
}

static bool ModifyAccessPathCost(THD *thd [[maybe_unused]],
                                 const JoinHypergraph &hypergraph
                                 [[maybe_unused]],
                                 AccessPath *path) {
  assert(!thd->is_error());
  assert(hypergraph.query_block()->join == hypergraph.join());
  AssertSupportedPath(path);
  return false;
}

static handler *Create(handlerton *hton, TABLE_SHARE *table_share, bool,
                       MEM_ROOT *mem_root) {
  return new (mem_root) rapid::ha_rapid(hton, table_share);
}

static int Init(MYSQL_PLUGIN p) {
  loaded_tables = new LoadedTables();

  handlerton *hton = static_cast<handlerton *>(p);
  hton->create = Create;
  hton->state = SHOW_OPTION_YES;
  hton->flags = HTON_IS_SECONDARY_ENGINE;
  hton->db_type = DB_TYPE_UNKNOWN;
  hton->prepare_secondary_engine = PrepareSecondaryEngine;
  hton->secondary_engine_pre_prepare_hook = SecondaryEnginePrePrepareHook;
  hton->optimize_secondary_engine = OptimizeSecondaryEngine;
  hton->secondary_engine_resolved = SecondaryEngineResolved;
  hton->compare_secondary_engine_cost = CompareJoinCost;
  hton->secondary_engine_flags =
      MakeSecondaryEngineFlags(SecondaryEngineFlag::SUPPORTS_HASH_JOIN,
                              SecondaryEngineFlag::USE_EXTERNAL_EXECUTOR);
  hton->secondary_engine_modify_view_ap_cost = ModifyAccessPathCost;

  /* The extensions that are linked into the plugin (core_functions: SUM, AVG
     and most of the other functions).  DuckDB registers them from a static
     initializer, which the linker leaves out of a static library. */
  duckdb_register_static_extensions();

  const char *db_path = rapid_database_file ? rapid_database_file : ":memory:";
  if (duckdb_open(db_path, &rapid::db) == DuckDBError) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Could not open DuckDB database");
    return 1;
  }
  
  // Initialize DuckDB settings
  duckdb_connection init_con;
  if (duckdb_connect(rapid::db, &init_con) == DuckDBSuccess) {
    // Enable automatic installation and loading of known extensions
    duckdb_query(init_con, "SET autoinstall_known_extensions=1", nullptr);
    duckdb_query(init_con, "SET autoload_known_extensions=1", nullptr);
    std::string sql = "ATTACH '" + std::string(db_path) + "' (COMPRESS true)";
    duckdb_query(init_con, sql.c_str(), nullptr); 
    
    duckdb_disconnect(&init_con);
  }
  
  // Initialize binlog consumer for incremental replication
  rapid_binlog_consumer_init();
  
  return 0;
}

static int Deinit(MYSQL_PLUGIN) {
  // Deinitialize binlog consumer
  rapid_binlog_consumer_deinit();
  
  delete loaded_tables;
  loaded_tables = nullptr;
  duckdb_close(&rapid::db);
  return 0;
}

// Combine RAPID system variables with binlog consumer system variables
static SYS_VAR **get_all_system_vars() {
  static SYS_VAR *all_vars[32];  // Enough space for both sets
  static bool initialized = false;
  
  if (!initialized) {
    int idx = 0;
    
    // Add RAPID system variables
    for (int i = 0; rapid_system_variables[i] != nullptr; i++) {
      all_vars[idx++] = rapid_system_variables[i];
    }
    
    // Add binlog consumer system variables
    st_mysql_sys_var **binlog_vars = rapid_binlog_get_system_vars();
    if (binlog_vars) {
      for (int i = 0; binlog_vars[i] != nullptr; i++) {
        all_vars[idx++] = reinterpret_cast<SYS_VAR*>(binlog_vars[i]);
      }
    }
    
    all_vars[idx] = nullptr;
    initialized = true;
  }
  
  return all_vars;
}

static st_mysql_storage_engine rapid_storage_engine{
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(rapid){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &rapid_storage_engine,
    "RAPID",
    PLUGIN_AUTHOR_ORACLE,
    "Rapid storage engine with binlog-based incremental replication",
    PLUGIN_LICENSE_GPL,
    Init,
    nullptr,
    Deinit,
    0x0001,
    reinterpret_cast<SHOW_VAR*>(rapid_binlog_get_status_vars()),  // Status variables (read-only)
    get_all_system_vars(),  // Combined system variables (RAPID + binlog consumer)
    nullptr,
    0,
} mysql_declare_plugin_end;
