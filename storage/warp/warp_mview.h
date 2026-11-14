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

#ifndef WARP_MVIEW_INCLUDED
#define WARP_MVIEW_INCLUDED

/**
  @file storage/warp/warp_mview.h

  The WARP_MVIEW daemon plugin: maintains the SYNCHRONOUS table-backed
  materialized views (see sql/mview.h) from the binary log of this server.

  warp_mview_daemon.cc  the plugin, its thread, the binary log reader and
                        the SQL that applies a batch of changed keys
  warp_mview_rows.cc    decodes the row images of row events into SQL
                        literals

  This header is included by ha_warp.cc (for the plugin declaration) and by
  the two files above; it must not pull in FastBit or the server internals.
*/

#include <string>
#include <vector>

#include "my_inttypes.h"
#include "mysql/plugin.h"
#include "mysql/status_var.h"

/* The second plugin of ha_warp.so, declared in ha_warp.cc */
extern struct st_mysql_daemon warp_mview_daemon_descriptor;
extern SHOW_VAR warp_mview_status_variables[];
extern SYS_VAR *warp_mview_system_variables[];
int warp_mview_plugin_init(void *p);
int warp_mview_plugin_deinit(void *p);

#ifdef WARP_MVIEW_INTERNAL

class Table_map_log_event;
class Rows_log_event;
class table_def;

namespace warp_mview {

/**
  What a Table_map event says about the columns of a table, in the form the
  row decoder needs.
*/
class Table_map_info {
 public:
  Table_map_info();
  ~Table_map_info();
  Table_map_info(const Table_map_info &) = delete;
  Table_map_info &operator=(const Table_map_info &) = delete;

  /**
    Read the event.
    @retval false the columns can be walked
    @retval true  the event is malformed or has columns whose row format is
                  not known here; the rows of the table cannot be decoded
  */
  bool init(Table_map_log_event *ev);

  std::string db;
  std::string table;
  unsigned long column_count{0};
  /// Parses the field metadata and knows the size of every field.
  table_def *def{nullptr};
  /// The type bytes of the event, before table_def translates them.
  std::vector<unsigned char> raw_type;
  /// Per column: 1 unsigned, 0 signed, -1 not a number or not known.
  std::vector<signed char> is_unsigned;
  /// Per column: the collation number, 0 when not a string or not known.
  std::vector<unsigned> collation;
  /// Number of JSON columns (the width of the partial bits of an update).
  unsigned json_columns{0};
};

enum class Cell_state : unsigned char {
  ABSENT,      ///< The column is not in this row image.
  SQL_NULL,    ///< The value is NULL (literal is "NULL").
  VALUE,       ///< literal is an SQL literal of the value.
  UNSUPPORTED  ///< No literal for this type; literal says why.
};

/** One column of one row image. */
struct Cell {
  Cell_state state{Cell_state::ABSENT};
  /// False when the literal does not compare equal to the stored value
  /// (FLOAT), so it must not be used to find the row.
  bool exact{true};
  std::string literal;
};

/**
  One changed row: the wanted columns of its before and after image.
  before is empty for an INSERT, after is empty for a DELETE; otherwise
  both have one cell per wanted column.
*/
struct Row_change {
  enum Kind { INSERT, UPDATE, DELETE } kind{INSERT};
  /// Set by the reader: which transaction of the batch the row belongs to
  /// (the transaction's end position in the binary log is kept there).
  unsigned trx{0};
  std::vector<Cell> before;
  std::vector<Cell> after;
};

/**
  Decode the rows of a row event, in the order of the event.

  @param map        the table map of the event's table
  @param ev         WRITE, UPDATE, DELETE or PARTIAL_UPDATE rows event
  @param columns    positions of the wanted columns among the columns of
                    the table (as in Mview_table::pk_ordinals)
  @param[out] rows  the rows are appended
  @param[out] why   on failure, what could not be decoded

  @retval false success
  @retval true  the rows cannot be walked (the layout of the event does not
                match the table map or the wanted columns); rows appended
                so far must not be used and the caller should refresh the
                views of the table completely.
*/
bool decode_rows(const Table_map_info &map, Rows_log_event *ev,
                 const std::vector<unsigned> &columns,
                 std::vector<Row_change> *rows, std::string *why);

}  // namespace warp_mview

#endif  // WARP_MVIEW_INTERNAL
#endif  // WARP_MVIEW_INCLUDED
