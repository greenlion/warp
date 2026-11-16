/* Copyright (c) 2026, WarpSQL contributors.

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

#ifndef STORAGE_RAPID_RAPID_SQL_H
#define STORAGE_RAPID_RAPID_SQL_H

#include <string>

class THD;
struct LEX;

namespace rapid {

/**
  Writes the statement of a resolved query for DuckDB (see rapid_sql.cc).

  @param thd       the session
  @param lex       the statement, prepared and not yet optimized
  @param[out] sql  the statement for DuckDB
  @param[out] why  what the statement has that can not be written
  @retval false  sql is set
  @retval true   the statement can not be run by DuckDB, why is set
*/
bool generate_duckdb_sql(THD *thd, LEX *lex, std::string *sql,
                         std::string *why);

}  // namespace rapid

#endif  // STORAGE_RAPID_RAPID_SQL_H
