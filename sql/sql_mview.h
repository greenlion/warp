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

#ifndef SQL_SQL_MVIEW_INCLUDED
#define SQL_SQL_MVIEW_INCLUDED

/**
  @file sql/sql_mview.h

  Server-internal part of table-backed materialized views: the statements
  (CREATE / DROP / REFRESH / ALTER MATERIALIZED VIEW), the data dictionary
  representation of the metadata and the guard that keeps ordinary DML and
  DDL away from the tables of a materialized view.

  The interface for the maintenance daemon is sql/mview.h.
*/

#include <string>
#include <vector>

#include "my_sqlcommand.h"
#include "sql/mview.h"
#include "sql/sql_cmd_ddl.h"

class String;
class THD;
struct TABLE_SHARE;
class Table_ref;

namespace dd {
class Abstract_table;
class Table;
}  // namespace dd

/** What a base table is to the materialized view machinery. */
enum Mview_table_kind : unsigned char {
  MVIEW_NOT_A_MVIEW = 0,
  MVIEW_VIEW_TABLE = 1,  ///< The table that holds the view's rows.
  MVIEW_STATE_TABLE = 2  ///< The binlog position table of an aggregate view.
};

/**
  Data dictionary option keys (dd::Table::options()).

  "mview"        1 on the table of a materialized view, 2 on the one-row
                 binlog position table of an aggregate view; absent on
                 every other table.
  "mview_def"    On the view table: the metadata, a JSON document.
*/
extern const char *const MVIEW_DD_KIND_KEY;
extern const char *const MVIEW_DD_DEF_KEY;

/** Prefix of every internal column / index name. User columns of a
    materialized view may not start with it. */
extern const char *const MVIEW_INTERNAL_PREFIX;
/** Suffix that makes the name of the binlog position table of an aggregate
    view from the name of the view. */
extern const char *const MVIEW_STATE_SUFFIX;

/** The kind recorded in the options of a dictionary object. */
Mview_table_kind mview_kind_from_dd(const dd::Abstract_table &table);

/**
  Read the metadata of a materialized view from the options of its table.
  Everything but Mview_table::pk_ordinals is filled in.

  @param db          Schema of the table.
  @param table       The dictionary object of the view table.
  @param[out] def    The definition.
  @param[out] user_columns  Names of the user visible columns (may be null).
  @param[out] reason Why the metadata could not be read.
  @retval false success
  @retval true  the table is not a materialized view or its metadata is
                broken; no error is reported, see reason.
*/
bool mview_def_from_dd(const char *db, const dd::Abstract_table &table,
                       Mview_def *def, std::vector<std::string> *user_columns,
                       std::string *reason);

/** The JSON document stored under MVIEW_DD_DEF_KEY. */
std::string mview_def_to_json(const Mview_def &def,
                              const std::vector<std::string> &user_columns);

/** `name`, with the backticks of the name doubled. */
std::string mview_quote_identifier(const std::string &name);

/**
  Fill in Mview_table::pk_ordinals of every table of the view from the
  current definitions of the base tables (shared metadata locks with
  transaction duration are taken on them).

  @param[out] reason  Why the view cannot be maintained any more.
  @param[out] error   True if an error was reported (lock wait timeout...),
                      false if only the view is unusable.
  @retval false success
  @retval true  see reason / error
*/
bool mview_fill_pk_ordinals(THD *thd, Mview_def *def, std::string *reason,
                            bool *error);

/**
  Refuse a statement of a user session that would change a table of a
  materialized view.

  @retval false the table is not part of a materialized view, or the session
                is in maintenance mode
  @retval true  error ER_MVIEW_IS_READ_ONLY has been reported
*/
bool mview_check_table_change(THD *thd, const char *db, const char *table_name,
                              Mview_table_kind kind);

/** Same, for a table whose dictionary object is at hand (DROP, RENAME,
    TRUNCATE do not open the table). */
bool mview_check_table_change(THD *thd, const char *db, const char *table_name,
                              const dd::Abstract_table *table_def);

/** DROP VIEW named a materialized view: report ER_MVIEW_USE_DROP_MVIEW. */
bool mview_check_drop_view(THD *thd, const char *db, const char *table_name,
                           const dd::Abstract_table *table_def);

/**
  SHOW CREATE VIEW for a materialized view.

  @param thd         Session.
  @param table_ref   The opened view table.
  @param[out] buffer "CREATE MATERIALIZED VIEW ..." in utf8mb4.
  @retval false success
  @retval true  error (reported)
*/
bool mview_show_create(THD *thd, Table_ref *table_ref, String *buffer);

/** Status: views filled by several sessions, and the sessions. */
extern unsigned long long mview_populate_parallel;
extern unsigned long long mview_populate_sessions;

/** Note in the registry that something changed (see mview_registry_version).
 */
void mview_registry_changed();

class Sql_cmd_create_mview final : public Sql_cmd_ddl {
 public:
  enum_sql_command sql_command_code() const override {
    return SQLCOM_CREATE_MATERIALIZED_VIEW;
  }
  bool execute(THD *thd) override;
};

class Sql_cmd_drop_mview final : public Sql_cmd_ddl {
 public:
  explicit Sql_cmd_drop_mview(bool if_exists) : m_if_exists(if_exists) {}
  enum_sql_command sql_command_code() const override {
    return SQLCOM_DROP_MATERIALIZED_VIEW;
  }
  bool execute(THD *thd) override;

 private:
  bool m_if_exists;
};

class Sql_cmd_refresh_mview final : public Sql_cmd_ddl {
 public:
  enum_sql_command sql_command_code() const override {
    return SQLCOM_REFRESH_MATERIALIZED_VIEW;
  }
  bool execute(THD *thd) override;
};

class Sql_cmd_alter_mview final : public Sql_cmd_ddl {
 public:
  explicit Sql_cmd_alter_mview(Mview_refresh_mode mode) : m_mode(mode) {}
  enum_sql_command sql_command_code() const override {
    return SQLCOM_ALTER_MATERIALIZED_VIEW;
  }
  bool execute(THD *thd) override;

 private:
  Mview_refresh_mode m_mode;
};

#endif  // SQL_SQL_MVIEW_INCLUDED
