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

#ifndef PLUGIN_SECONDARY_ENGINE_RAPID_HA_RAPID_H_
#define PLUGIN_SECONDARY_ENGINE_RAPID_HA_RAPID_H_

#include "my_base.h"
#include "sql/handler.h"
#include "thr_lock.h"
#include "duckdb.h"
#include <map>
#include <string>

class THD;
struct TABLE;
struct TABLE_SHARE;
struct LEX;

namespace dd {
class Table;
}

namespace rapid {

duckdb_database db;

class ha_rapid : public handler {
 public:
  ha_rapid(handlerton *hton, TABLE_SHARE *table_share);

 private:

  int create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) override {
    return HA_ERR_WRONG_COMMAND;
  }

  int open(const char *name, int mode, unsigned int test_if_locked,
           const dd::Table *table_def) override;

  int close() override { return 0; }

  int rnd_init(bool) override { return 0; }

  int rnd_next(unsigned char *) override { return HA_ERR_END_OF_FILE; }

  int rnd_pos(unsigned char *, unsigned char *) override {
    return HA_ERR_WRONG_COMMAND;
  }

  int info(unsigned int) override;

  ha_rows records_in_range(unsigned int index, key_range *min_key,
                           key_range *max_key) override;

  void position(const unsigned char *) override {}

  unsigned long index_flags(unsigned int, unsigned int, bool) const override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;

  Table_flags table_flags() const override;

  const char *table_type() const override { return "RAPID"; }

  /**
   * Load table into the secondary engine.
   *
   * @param[in] table - table to be loaded
   * @param[out] skip_metadata_update - should the DD metadata be updated for
   * the load of this table
   * @return 0 if success
   */
  int load_table(const TABLE &table, bool *skip_metadata_update) override;

  /**
   * Unload the table from secondary engine
   *
   * @param[in] db_name     database name
   * @param[in] table_name  table name
   * @param[in] error_if_not_loaded - whether to report an error if the table is
   * already not present in the secondary engine.
   * @return 0 if success
   */
  int unload_table(const char *db_name, const char *table_name,
                   bool error_if_not_loaded) override;

  std::string escape_string(std::string in);
  int create_duckdb_table(const TABLE &table, duckdb_connection con);

                    
  THR_LOCK_DATA m_lock;
};

/**
  Convert a LEX* into the SQL string that originated the AST.
  
  @param lex The LEX object containing the parsed AST
  @return The original SQL query string as a LEX_CSTRING, or NULL_CSTR if invalid
*/
LEX_CSTRING lex_to_sql_string(LEX *lex);

/**
  Reconstruct SQL from LEX AST with DuckDB-compatible table name transformation.
  
  @param lex The LEX object containing the parsed AST
  @return The reconstructed SQL query as a std::string, or empty string if invalid
*/
std::string lex_to_duckdb_sql(LEX *lex);

/**
  Transform table references from SCHEMA.TABLE to SCHEMA_TABLE format.
  
  @param sql The original SQL string
  @return The transformed SQL string with DuckDB-compatible table names
*/
std::string transform_table_references(const std::string& sql);

/**
  Transform table references from SCHEMA.TABLE to SCHEMA_TABLE format and add implicit aliases.
  
  @param sql The original SQL string
  @param table_aliases Map from full table names to their aliases
  @return The transformed SQL string with DuckDB-compatible table names and implicit aliases
*/
std::string transform_table_references_with_aliases(const std::string& sql, 
                                                  const std::map<std::string, std::string>& table_aliases);

/**
  Collect table aliases from the LEX AST.
  
  @param lex The LEX object containing the parsed AST
  @return A map from full table names to their aliases
*/
std::map<std::string, std::string> collect_table_aliases(LEX *lex);

/**
  Replace full table names with aliases in SELECT clauses.
  
  @param sql The SQL string
  @param table_aliases Map from full table names to aliases
  @return The SQL string with table names replaced by aliases in SELECT clauses
*/
std::string replace_table_names_with_aliases(const std::string& sql, 
                                           const std::map<std::string, std::string>& table_aliases);

/**
  Strip backticks from SELECT clauses for DuckDB compatibility.
  
  @param sql The SQL string
  @return The SQL string with backticks removed from SELECT clauses
*/
std::string strip_backticks_from_select(const std::string& sql);

}  // namespace rapid

#endif  // PLUGIN_SECONDARY_ENGINE_RAPID_HA_RAPID_H_
