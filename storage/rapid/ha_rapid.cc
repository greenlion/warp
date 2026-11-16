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
#include "duckdb.h"
#include "sql/protocol.h"

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

static SYS_VAR *rapid_system_variables[] = {
    MYSQL_SYSVAR(database_file),
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

/**
  Reconstruct SQL from LEX AST with DuckDB-compatible table name transformation.
  
  This function traverses the LEX AST and reconstructs the SQL query, transforming
  schema.table references to schema_table format for DuckDB compatibility.
  It also uses table aliases in SELECT clauses instead of full table names.
  
  @param lex The LEX object containing the parsed AST
  @return The reconstructed SQL query as a std::string, or empty string if invalid
  
  @note This function handles SELECT, INSERT, UPDATE, DELETE statements and
        transforms table references from SCHEMA.TABLE to SCHEMA_TABLE format.
        It also replaces full table names with aliases in SELECT clauses.
        
  @example
    LEX *lex = thd->lex;
    std::string reconstructed_sql = lex_to_duckdb_sql(lex);
*/
std::string lex_to_duckdb_sql(LEX *lex) {
  if (lex == nullptr || lex->thd == nullptr) {
    return "";
  }
  
  String sql_buffer;
  THD *thd = lex->thd;
  
  // Use MySQL's built-in print functionality to reconstruct the SQL
  // This handles all the complex cases like subqueries, joins, etc.
  if (lex->unit != nullptr) {
    lex->unit->print(thd, &sql_buffer, QT_ORDINARY);
  } else if (lex->query_block != nullptr) {
    lex->query_block->print(thd, &sql_buffer, QT_ORDINARY);
  } else {
    return "";
  }
  
  std::string result = std::string(sql_buffer.ptr(), sql_buffer.length());
  
  // First, collect table aliases from the LEX
  std::map<std::string, std::string> table_aliases = collect_table_aliases(lex);
  
  // Replace full table names with aliases everywhere
  std::string sql_with_aliases = replace_table_names_with_aliases(result, table_aliases);
  
  // Transform schema.table references to schema_table format in FROM/JOIN clauses only
  std::string transformed_sql = transform_table_references_with_aliases(sql_with_aliases, table_aliases);
  
  // Strip backticks for DuckDB compatibility (DuckDB uses double quotes, not backticks)
  std::string duckdb_compatible_sql = transformed_sql;
  
  // Remove all backticks from the query
  size_t pos = 0;
  while ((pos = duckdb_compatible_sql.find('`', pos)) != std::string::npos) {
    duckdb_compatible_sql.erase(pos, 1);
  }
  
  // Quote aliases that need quoting (contain special chars like *, (), etc.)
  // Pattern: " AS identifier" where identifier contains special characters
  std::string upper_sql = duckdb_compatible_sql;
  std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(), ::toupper);
  
  pos = 0;
  while ((pos = upper_sql.find(" AS ", pos)) != std::string::npos) {
    size_t alias_start = pos + 4; // Skip " AS "
    
    // Skip whitespace after AS
    while (alias_start < duckdb_compatible_sql.length() && 
           std::isspace(duckdb_compatible_sql[alias_start])) {
      alias_start++;
    }
    
    // Find the end of the alias (until comma, whitespace, FROM, WHERE, GROUP, ORDER, or end)
    size_t alias_end = alias_start;
    bool needs_quoting = false;
    
    while (alias_end < duckdb_compatible_sql.length()) {
      char ch = duckdb_compatible_sql[alias_end];
      if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        break;
      }
      // Check if character needs quoting (special chars like *, (), etc.)
      if (!std::isalnum(ch) && ch != '_') {
        needs_quoting = true;
      }
      alias_end++;
    }
    
    // If alias needs quoting and isn't already quoted, add double quotes
    if (needs_quoting && alias_start < alias_end) {
      std::string alias = duckdb_compatible_sql.substr(alias_start, alias_end - alias_start);
      
      // Check if already quoted
      if (!(alias.front() == '"' && alias.back() == '"')) {
        std::string quoted_alias = "\"" + alias + "\"";
        duckdb_compatible_sql.replace(alias_start, alias_end - alias_start, quoted_alias);
        
        // Update upper_sql for next iteration
        upper_sql = duckdb_compatible_sql;
        std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(), ::toupper);
        
        pos = alias_start + quoted_alias.length();
      } else {
        pos = alias_end;
      }
    } else {
      pos = alias_end;
    }
  }
  
  // Fix implicit joins: Convert JOIN without ON clause to comma-separated table list
  // "from table1 t1 join table2 t2 join table3 t3 where ..." 
  // becomes "from table1 t1, table2 t2, table3 t3 where ..."
  std::string fixed_sql = duckdb_compatible_sql;
  std::string upper_fixed = fixed_sql;
  std::transform(upper_fixed.begin(), upper_fixed.end(), upper_fixed.begin(), ::toupper);
  
  // Find the FROM clause
  size_t from_pos = upper_fixed.find(" FROM ");
  if (from_pos != std::string::npos) {
    // Find the end of the FROM clause (WHERE, GROUP BY, ORDER BY, LIMIT, or end of string)
    size_t from_end = from_pos + 6; // Skip " FROM "
    std::vector<std::string> end_keywords = {"WHERE", "GROUP", "ORDER", "LIMIT", "HAVING"};
    size_t clause_end = fixed_sql.length();
    
    for (const auto& keyword : end_keywords) {
      size_t kw_pos = upper_fixed.find(" " + keyword + " ", from_end);
      if (kw_pos != std::string::npos && kw_pos < clause_end) {
        clause_end = kw_pos;
      }
    }
    
    // Extract the FROM clause content
    std::string from_clause = fixed_sql.substr(from_end, clause_end - from_end);
    std::string upper_from = from_clause;
    std::transform(upper_from.begin(), upper_from.end(), upper_from.begin(), ::toupper);
    
    // Replace " join " with ", " if not followed by "ON"
    std::string modified_from = from_clause;
    pos = 0;
    while ((pos = upper_from.find(" JOIN ", pos)) != std::string::npos) {
      // Check if followed by ON clause
      size_t after_join = pos + 6;
      // Skip whitespace and table name/alias to see what comes next
      size_t check_pos = after_join;
      int word_count = 0;
      while (check_pos < upper_from.length() && word_count < 2) {
        // Skip whitespace
        while (check_pos < upper_from.length() && std::isspace(upper_from[check_pos])) {
          check_pos++;
        }
        // Skip word (table name or alias)
        while (check_pos < upper_from.length() && 
               (std::isalnum(upper_from[check_pos]) || upper_from[check_pos] == '_')) {
          check_pos++;
        }
        word_count++;
      }
      
      // Check if "ON" follows
      while (check_pos < upper_from.length() && std::isspace(upper_from[check_pos])) {
        check_pos++;
      }
      
      bool has_on_clause = (check_pos + 2 <= upper_from.length() && 
                           upper_from.substr(check_pos, 2) == "ON");
      
      if (!has_on_clause) {
        // Replace " join " with ", "
        modified_from.replace(pos, 6, ", ");
        upper_from = modified_from;
        std::transform(upper_from.begin(), upper_from.end(), upper_from.begin(), ::toupper);
        pos += 2;  // Skip ", "
      } else {
        pos += 6;  // Skip " JOIN "
      }
    }
    
    // Replace the FROM clause in the original SQL
    fixed_sql.replace(from_end, clause_end - from_end, modified_from);
  }
  
  return fixed_sql;
}

/**
  No transformation needed - keep schema.table format as is.

  Previously this function transformed SCHEMA.TABLE to SCHEMA_TABLE format,
  but now we use DuckDB schemas directly, so no transformation is needed.

  @param sql The original SQL string
  @return The SQL string unchanged
*/
std::string transform_table_references(const std::string& sql) {
  // No transformation needed - DuckDB supports schema.table format directly
  return sql;
}

/**
  Add implicit aliases to table references that don't have explicit aliases.

  This function ensures tables in FROM clauses, JOIN clauses, etc. have proper aliases
  while preserving the schema.table format (no transformation to underscore format).

  @param sql The original SQL string
  @param table_aliases Map from full table names (schema.table) to their aliases
  @return The SQL string with implicit aliases added where needed
*/
std::string transform_table_references_with_aliases(const std::string& sql,
                                                  const std::map<std::string, std::string>& table_aliases) {
  // With schema.table format, we don't need to transform anything
  // MySQL's query rewriter already handles the schema.table references correctly
  // and DuckDB will understand them directly
  return sql;
}

/**
  Collect table aliases from the LEX AST.
  
  @param lex The LEX object containing the parsed AST
  @return A map from full table names to their aliases
*/
std::map<std::string, std::string> collect_table_aliases(LEX *lex) {
  std::map<std::string, std::string> aliases;
  
  if (lex == nullptr || lex->query_block == nullptr) {
    return aliases;
  }
  
  // Traverse all table references in the query block
  for (Table_ref *table = lex->query_block->get_table_list(); table != nullptr;
       table = table->next_local) {

    // Build the full table name (schema.table format)
    std::string full_table_name;
    if (table->db != nullptr && table->table_name != nullptr) {
      full_table_name = std::string(table->db) + "." + std::string(table->table_name);
    } else if (table->table_name != nullptr) {
      full_table_name = std::string(table->table_name);
    }

    if (!full_table_name.empty()) {
      std::string alias;

      if (table->alias != nullptr) {
        // Use explicit alias if provided
        alias = std::string(table->alias);
      } else if (table->table_name != nullptr) {
        // Use table name as implicit alias when no explicit alias is provided
        alias = std::string(table->table_name);
      }

      if (!alias.empty()) {
        aliases[full_table_name] = alias;
      }
    }
  }
  
  return aliases;
}

/**
  Replace full table names with aliases throughout the SQL query.
  
  This handles column references in all clauses (SELECT, WHERE, JOIN, ORDER BY, etc.)
  by replacing patterns like `db`.`table`.`column` with `alias`.`column`.
  
  @param sql The SQL string
  @param table_aliases Map from full table names (db_table) to aliases
  @return The SQL string with table names replaced by aliases
*/
std::string replace_table_names_with_aliases(const std::string& sql, 
                                           const std::map<std::string, std::string>& table_aliases) {
  std::string result = sql;
  
  // Handle the original format (db.table.column) that MySQL's print() generates
  // We need to replace these patterns globally, not just in SELECT clauses
  for (const auto& alias_pair : table_aliases) {
    const std::string& full_name = alias_pair.first;  // e.g., "test.dim_date"
    const std::string& alias = alias_pair.second;      // e.g., "dim_date"

    size_t dot_pos = full_name.find('.');
    if (dot_pos != std::string::npos) {
      std::string db_name = full_name.substr(0, dot_pos);           // e.g., "test"
      std::string table_name = full_name.substr(dot_pos + 1);       // e.g., "dim_date"

      // Pattern 1: `db`.`table`.`column` -> `alias`.`column`
      std::string pattern1 = "`" + db_name + "`.`" + table_name + "`.";
      std::string replacement1 = "`" + alias + "`.";

      size_t pos = 0;
      while ((pos = result.find(pattern1, pos)) != std::string::npos) {
        result.replace(pos, pattern1.length(), replacement1);
        pos += replacement1.length();
      }

      // Pattern 2: db.table.column (without backticks) -> alias.column
      // This handles cases where MySQL prints without quotes
      std::string pattern2 = db_name + "." + table_name + ".";
      std::string replacement2 = alias + ".";

      pos = 0;
      while ((pos = result.find(pattern2, pos)) != std::string::npos) {
        // Make sure we're not matching something like "mydb.table.field" inside a string literal
        // Simple check: if preceded by quote, skip it
        if (pos > 0 && (result[pos-1] == '\'' || result[pos-1] == '"')) {
          pos++;
          continue;
        }
        result.replace(pos, pattern2.length(), replacement2);
        pos += replacement2.length();
      }
    }
  }
  
  return result;
}

/**
  Strip backticks from SELECT clauses for DuckDB compatibility.
  
  DuckDB doesn't support backtick-quoted identifiers, so we need to remove them
  from the SELECT clause while preserving them in other parts of the query.
  
  @param sql The SQL string
  @return The SQL string with backticks removed from SELECT clauses
*/
std::string strip_backticks_from_select(const std::string& sql) {
  std::string result = sql;
  
  // Case-insensitive scan for SELECT ... FROM ranges
  std::string upper_result = result;
  std::transform(upper_result.begin(), upper_result.end(), upper_result.begin(), ::toupper);
  size_t scan_pos = 0;
  while (true) {
    size_t select_pos = upper_result.find("SELECT", scan_pos);
    if (select_pos == std::string::npos) break;
    if (select_pos > 0 && std::isalnum(static_cast<unsigned char>(upper_result[select_pos - 1]))) {
      scan_pos = select_pos + 1;
      continue;
    }
    size_t from_pos_upper = upper_result.find(" FROM ", select_pos);
    if (from_pos_upper == std::string::npos) break;

    size_t select_start_content = select_pos + 6;
    std::string select_clause = result.substr(select_start_content, from_pos_upper - select_start_content);

    std::string modified_select = select_clause;
    
    // First, strip backticks
    size_t pos = 0;
    while ((pos = modified_select.find('`', pos)) != std::string::npos) {
      modified_select.erase(pos, 1);
    }
    
    // Second, quote aliases that need quoting (contain special chars like *, (), etc.)
    // Pattern: " AS identifier" where identifier contains special characters
    std::string upper_modified = modified_select;
    std::transform(upper_modified.begin(), upper_modified.end(), upper_modified.begin(), ::toupper);
    
    pos = 0;
    while ((pos = upper_modified.find(" AS ", pos)) != std::string::npos) {
      size_t alias_start = pos + 4; // Skip " AS "
      
      // Skip whitespace after AS
      while (alias_start < modified_select.length() && std::isspace(modified_select[alias_start])) {
        alias_start++;
      }
      
      // Find the end of the alias (until comma, whitespace after non-identifier char, or end)
      size_t alias_end = alias_start;
      bool needs_quoting = false;
      bool in_identifier = true;
      
      while (alias_end < modified_select.length() && in_identifier) {
        char ch = modified_select[alias_end];
        if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
          break;
        }
        // Check if character needs quoting (special chars like *, (), etc.)
        if (!std::isalnum(ch) && ch != '_') {
          needs_quoting = true;
        }
        alias_end++;
      }
      
      // If alias needs quoting and isn't already quoted, add double quotes
      if (needs_quoting && alias_start < alias_end) {
        std::string alias = modified_select.substr(alias_start, alias_end - alias_start);
        
        // Check if already quoted
        if (!(alias.front() == '"' && alias.back() == '"')) {
          std::string quoted_alias = "\"" + alias + "\"";
          modified_select.replace(alias_start, alias_end - alias_start, quoted_alias);
          
          // Update upper_modified for next iteration
          upper_modified = modified_select;
          std::transform(upper_modified.begin(), upper_modified.end(), upper_modified.begin(), ::toupper);
          
          pos = alias_start + quoted_alias.length();
        } else {
          pos = alias_end;
        }
      } else {
        pos = alias_end;
      }
    }

    if (modified_select != select_clause) {
      result.replace(select_start_content, from_pos_upper - select_start_content, modified_select);
      upper_result = result;
      std::transform(upper_result.begin(), upper_result.end(), upper_result.begin(), ::toupper);
      scan_pos = select_start_content + modified_select.length();
    } else {
      scan_pos = from_pos_upper + 6;
    }
  }
  
  return result;
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
                                      Field *field) {
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
      field->store_time(&mysql_time);
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
      if (field->type() == MYSQL_TYPE_TIMESTAMP) {
        /* a point in time: the date and time of the time zone of the server
           (see rapid_server_time_zone), which the field shows in the time
           zone of the session */
        bool in_dst_time_gap = false;
        my_timeval tv;
        tv.m_tv_sec = rapid_server_time_zone()->TIME_to_gmt_sec(
            &mysql_time, &in_dst_time_gap);
        tv.m_tv_usec = ts_struct.time.micros;
        field->store_timestamp(&tv);
      } else {
        field->store_time(&mysql_time);
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
  
  // Execute the query
  duckdb_result result;
  if (duckdb_query(con, duckdb_sql.c_str(), &result) == DuckDBError) {
    const char *error = duckdb_result_error(&result);
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             error ? error : "DuckDB query execution failed");
    duckdb_destroy_result(&result);
    duckdb_disconnect(&con);
    return true;
  }
  
  // Get the result set metadata
  idx_t row_count = duckdb_row_count(&result);
  idx_t column_count = duckdb_column_count(&result);
  
  // Get the field list from the JOIN
  mem_root_deque<Item *> *fields = join->fields;
  if (fields == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Could not get field list from JOIN");
    duckdb_destroy_result(&result);
    duckdb_disconnect(&con);
    return true;
  }
  
  // For queries with aggregates or expressions without fields, we need to set up result fields
  // Check if we have tmp_table_param with items_to_copy
  if (join->tmp_table_param.items_to_copy != nullptr) {
    // Map output items to their corresponding items in items_to_copy which have fields
    idx_t col = 0;
    auto &items_to_copy = *join->tmp_table_param.items_to_copy;
    
    for (Item *item : *fields) {
      if (col < items_to_copy.size()) {
        // Get the source item from items_to_copy
        Item *src_item = items_to_copy[col].func();
        if (src_item) {
          // If the source has a tmp_table_field, use it for our output item
          Field *src_field = src_item->get_tmp_table_field();
          if (src_field && item->get_tmp_table_field() == nullptr) {
            // Set this field as the result field for the output item
            item->set_result_field(src_field);
          }
        }
      }
      col++;
    }
  }
  
  // If no items_to_copy (direct execution without temp table), we need to handle differently
  // For items without fields, we'll store values in a protocol-compatible way
  bool has_items_without_fields = false;
  for (Item *item : *fields) {
    if (item->get_tmp_table_field() == nullptr && item->get_result_field() == nullptr) {
      has_items_without_fields = true;
      break;
    }
  }
  
  // If we have items without fields, the query should have used a temp table but didn't
  // This happens when MySQL optimizes the query differently
  // We need to send results directly through the protocol
  if (has_items_without_fields) {
    // Get the protocol object to send results directly
    Protocol *protocol = thd->get_protocol();
    
    // Send each row
    for (idx_t row = 0; row < row_count; ++row) {
      protocol->start_row();
      
      for (idx_t col = 0; col < column_count && col < fields->size(); ++col) {
        if (duckdb_value_is_null(&result, col, row)) {
          protocol->store_null();
        } else {
          // Get type and send appropriate value
          duckdb_type col_type = duckdb_column_type(&result, col);
          switch (col_type) {
            case DUCKDB_TYPE_BIGINT:
            case DUCKDB_TYPE_INTEGER:
            case DUCKDB_TYPE_SMALLINT:
            case DUCKDB_TYPE_TINYINT:
            case DUCKDB_TYPE_HUGEINT:
            case DUCKDB_TYPE_UBIGINT:
            case DUCKDB_TYPE_UINTEGER:
            case DUCKDB_TYPE_USMALLINT:
            case DUCKDB_TYPE_UTINYINT: {
              int64_t val = duckdb_value_int64(&result, col, row);
              protocol->store_longlong(val, false);
              break;
            }
            case DUCKDB_TYPE_DOUBLE:
            case DUCKDB_TYPE_FLOAT: {
              double val = duckdb_value_double(&result, col, row);
              protocol->store_double(val, DECIMAL_NOT_SPECIFIED, 0);
              break;
            }
            default: {
              char *val = duckdb_value_varchar(&result, col, row);
              if (val) {
                protocol->store_string(val, strlen(val), &my_charset_utf8mb4_0900_ai_ci);
                duckdb_free(val);
              } else {
                protocol->store_null();
              }
              break;
            }
          }
        }
      }
      
      if (protocol->end_row()) {
        duckdb_destroy_result(&result);
        duckdb_disconnect(&con);
        return true;
      }
      
      thd->inc_sent_row_count(1);
      join->send_records++;
    }
    
    duckdb_destroy_result(&result);
    duckdb_disconnect(&con);
    return false;
  }
  
  // Stream each row to MySQL
  for (idx_t row = 0; row < row_count; ++row) {
    // Convert DuckDB row to MySQL fields
    idx_t col = 0;
    for (Item *item : *fields) {
      if (col >= column_count) break;
      
      // Try multiple ways to get a Field object
      Field *field = item->get_tmp_table_field();
      if (field == nullptr) {
        field = item->get_result_field();
      }
      if (field == nullptr && item->type() == Item::FIELD_ITEM) {
        field = down_cast<Item_field *>(item)->field;
      }
      
      if (field != nullptr) {
        if (ConvertDuckDBValueToField(&result, col, row, field)) {
          duckdb_destroy_result(&result);
          duckdb_disconnect(&con);
          return true;
        }
      }
      
      ++col;
    }
    
    // Send the row to the client
    if (query_result->send_data(thd, *fields)) {
      duckdb_destroy_result(&result);
      duckdb_disconnect(&con);
      return true;
    }
    
    // Update row count
    thd->inc_sent_row_count(1);
    join->send_records++;
  }
  
  // Cleanup
  duckdb_destroy_result(&result);
  duckdb_disconnect(&con);
  
  return false;
}

static bool OptimizeSecondaryEngine(THD *thd [[maybe_unused]], LEX *lex) {
  // The context should have been set by PrepareSecondaryEngine.
  assert(lex->secondary_engine_execution_context() != nullptr);

  // Reconstruct SQL with DuckDB-compatible table names
  std::string duckdb_sql = rapid::lex_to_duckdb_sql(lex);
  if (!duckdb_sql.empty()) {
    // Store the DuckDB query in the execution context for later execution
    auto *context = down_cast<Rapid_execution_context *>(
        lex->secondary_engine_execution_context());
    context->SetDuckDBQuery(duckdb_sql);
    
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
