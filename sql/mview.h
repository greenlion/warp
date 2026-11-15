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

#ifndef SQL_MVIEW_INCLUDED
#define SQL_MVIEW_INCLUDED

/**
  @file sql/mview.h

  Table-backed materialized views.

  A materialized view is an ordinary base table (in any storage engine,
  chosen by ENGINE= or @@default_storage_engine) whose data dictionary
  options mark it as a materialized view and carry the metadata below.
  The server side (sql/mview.cc, sql/sql_mview.cc) owns the DDL and the
  metadata.  The maintenance daemon (storage/warp/warp_mview_*.cc) reads
  the binary log and keeps the tables up to date using only this header.

  Storage layout
  --------------
  Join view (no aggregation): the view table V holds the user's columns
  plus one set of INVISIBLE columns per joined table instance holding that
  instance's row key (NULL for a NULL-extended outer join row), see
  Mview_key_kind.  A key column that the user's columns already hold is
  not stored twice.

  Aggregate view (GROUP BY, aggregate functions or SELECT DISTINCT): V
  holds ONE ROW PER GROUP and nothing per base row:
    - INVISIBLE group columns, one per GROUP BY expression;
    - an INVISIBLE row count column (COUNT(*) of the group);
    - INVISIBLE state columns, one per Mview_agg_state;
    - the user's columns, as STORED generated columns computed from the
      columns above (e.g. AVG(x) = sum state / count state).
  A one-row state table next to V (same engine) records the binary log
  position up to which V is exact.

  Maintenance of a join view (recompute by root key)
  --------------------------------------------------
  The "root" is a table instance that is never NULL-extended, so every
  view row carries a non-NULL root key.  For a batch of changed base rows
  the daemon maps the old row keys of changed rows to root keys
  through V's key columns and the new row keys through the view's own
  join (with <=> where a key column is nullable), deletes the rows of those root keys and re-inserts them with the
  detail select restricted to those root keys.  Idempotent.

  Maintenance of an aggregate view
  --------------------------------
  The daemon loads the old (sign -1) and new (sign +1) row images of each
  changed base row into a per-instance delta table with the instance's
  ref_columns plus a sign column, and substitutes delta tables for base
  tables in from_template.

  delta_capable views (inner joins only; every state is COUNT_STAR, COUNT,
  SUM, MIN or MAX): the change of each group's row count and states is
  the signed sum, over the non-empty subsets U of changed instances, of
  the join with delta tables for U and base tables for the rest, weighted
  by (-1)^(|U|+1) times the product of the signs.  It is added to V.  A
  group whose MIN/MAX state may have lost its extreme value is recomputed
  instead.  Groups whose row count reaches zero are deleted.

  Other views (outer joins, RECOMPUTE states): the groups that may have
  changed are found by evaluating group_exprs over the same substituted
  joins (also with NULL-supplying instances replaced by empty sources),
  and each such group is deleted and recomputed from the base tables.

  Whenever base tables are read (every join, every recompute) they must
  be exactly as of the end of the batch: the daemon takes shared read
  locks on the base tables, notes the end of the binary log, opens a
  consistent snapshot, releases the locks, and reads the binary log up to
  the noted position before applying.  Relative updates are not
  idempotent, so the state table row is updated in the same transaction
  as V and changes at or before the recorded position are never applied
  again.  CREATE and REFRESH record the position the same way.
*/

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class THD;

enum class Mview_refresh_mode {
  MANUAL = 0,     ///< Only REFRESH MATERIALIZED VIEW changes the contents.
  SYNCHRONOUS = 1 ///< Maintained by the binlog daemon.
};

/**
  What the row key of a base table instance (Mview_table::pk_columns) is.

  A join view is maintained by recomputing the view rows of the root keys
  that changed (see above).  That is correct for any set of columns of the
  root: every view row whose root has one of the keys is deleted and every
  row of the join whose root has one of them is inserted again, so a key
  that is not unique only makes more rows be recomputed, and duplicate
  rows of a base table come and go together.  The same holds for the keys
  of the other instances, which are only mapped to root keys.  A key is
  therefore chosen for every table, primary key or not.
*/
enum class Mview_key_kind {
  PRIMARY,  ///< the PRIMARY KEY of the table
  UNIQUE,   ///< a UNIQUE key of NOT NULL columns (no PRIMARY KEY)
  COLUMNS,  ///< no such key: every column whose values compare exactly
            ///< (not FLOAT, BLOB, TEXT, JSON, spatial, generated)
  NONE      ///< aggregate views of tables without such a key: none
};

/** One instance (alias) of a base table in the view's FROM clause. */
struct Mview_table {
  std::string db;     ///< Base table schema.
  std::string table;  ///< Base table name.
  std::string alias;  ///< Alias used in the SQL fragments below.
  /// The row key of the instance (see Mview_key_kind): column names in
  /// the base table, in key order.  Empty for Mview_key_kind::NONE, and
  /// for COLUMNS when no column qualifies: then every change of the table
  /// refreshes the view completely.
  std::vector<std::string> pk_columns;
  Mview_key_kind key_kind{Mview_key_kind::PRIMARY};
  /// Some column of pk_columns is nullable in the current definition of
  /// the table: keys are compared with <=>.  Filled in at load time.
  bool key_nullable{false};
  /// 0-based positions of pk_columns among the base table's columns, as
  /// they appear in a binlog Table_map event.  Filled in from the current
  /// table definition when the metadata is loaded.
  std::vector<unsigned> pk_ordinals;
  /// Join views: names of the columns of V that hold this instance's row
  /// key; same order as pk_columns.  An INVISIBLE column of its own, or a
  /// column of the user that is exactly that column of the instance.
  std::vector<std::string> key_columns;
  /// Aggregate views: every column of this instance that the view
  /// references anywhere (select list, ON, WHERE, GROUP BY), plus the
  /// primary key columns, and their 0-based positions in a Table_map
  /// event (filled in at load time like pk_ordinals).
  std::vector<std::string> ref_columns;
  std::vector<unsigned> ref_ordinals;
  /// True if the instance is on the NULL-supplying side of an outer join.
  bool null_supplying{false};
};

/** One stored aggregate state of an aggregate view. */
struct Mview_agg_state {
  enum Kind {
    COUNT_STAR,  ///< unused: the row count column plays this role
    COUNT,       ///< COUNT(arg): number of rows with non-NULL arg
    SUM,         ///< SUM(arg), NULL when no row has a non-NULL arg; always
                 ///< paired with a COUNT state of the same arg
    MIN,         ///< MIN(arg)
    MAX,         ///< MAX(arg)
    RECOMPUTE    ///< any other aggregate; only full_sql is meaningful
  };
  Kind kind{RECOMPUTE};
  /// INVISIBLE column of V holding the state.
  std::string column;
  /// The aggregate's argument as SQL over the join aliases.
  std::string arg_sql;
  /// The complete aggregate as SQL over the join aliases, e.g.
  /// "SUM(`a`.`x`)" or "COUNT(DISTINCT `a`.`x`)": what REFRESH and a
  /// group recompute select for this column.
  std::string full_sql;
  /// For SUM: index in Mview_def::states of the COUNT state of the same
  /// argument.
  int count_state{-1};
};

/**
  Everything the daemon needs to maintain one materialized view.

  All SQL fragments are complete, fully qualified (schema.table and
  alias.column), printed by the server in a form that parses under
  sql_mode='' and any current schema, and never end in a semicolon.
*/
struct Mview_def {
  std::string db;
  std::string name;
  std::string engine;
  Mview_refresh_mode refresh_mode{Mview_refresh_mode::MANUAL};
  /// Changes every time the view is created, altered or fully refreshed.
  uint64_t epoch{0};
  /// The SELECT as the user wrote it (for SHOW CREATE and diagnostics).
  std::string definition;

  bool is_aggregate{false};

  /// Table instances in FROM-clause order.
  std::vector<Mview_table> tables;
  /// Index into tables of the root instance (join views).
  unsigned root{0};

  /// "FROM <joins>" of the view, with aliases as in tables[].alias.
  std::string from_sql;
  /// from_sql with the base table of instance N printed as the quoted
  /// identifier `mv$source$N` (no schema), still followed by its alias:
  /// "FROM `mv$source$0` `a` LEFT JOIN `mv$source$1` `b` ON (...)".
  /// The daemon replaces each token by `db`.`table`, a delta table or a
  /// derived table.
  std::string from_template;
  /// The view's WHERE condition without the keyword; empty if none.
  std::string where_sql;

  // ---- Join views only ----
  /// Column list of V, comma separated and quoted, matching detail_select
  /// position by position.  Includes the key columns.
  std::string detail_columns;
  /// "SELECT <exprs>" over the join, without FROM; one expression per
  /// column of detail_columns.
  std::string detail_select;

  // ---- Aggregate views only ----
  /// True if the view can be maintained by adding deltas (see above).
  bool delta_capable{false};
  /// The GROUP BY expressions as SQL over the join aliases.  Empty for an
  /// aggregate without GROUP BY, which always has exactly one row.
  std::vector<std::string> group_exprs;
  /// INVISIBLE columns of V holding the group expressions; same order.
  std::vector<std::string> view_group_columns;
  /// INVISIBLE column of V holding the group's row count.
  std::string count_column;
  /// Aggregate states, in the order of their columns in V.
  std::vector<Mview_agg_state> states;
  /// One-row table (same engine as V) with columns
  /// (binlog_file VARCHAR, binlog_pos BIGINT UNSIGNED): V is exact as of
  /// this position.
  std::string state_db;
  std::string state_name;
};

/**
  Incremented whenever a materialized view is created, dropped, altered or
  refreshed.  The daemon reloads its registry when the value changes.
*/
extern std::atomic<uint64_t> mview_registry_version;

/**
  Load the definitions of all materialized views from the data dictionary.

  @param thd        Session with no open tables and no active transaction.
  @param[out] defs  The views; views whose metadata cannot be read are
                    skipped with a warning in the error log.
  @retval false success
  @retval true  error (reported in the diagnostics area)
*/
bool mview_load_all(THD *thd, std::vector<Mview_def> *defs);

/**
  The views whose contents the maintenance daemon keeps up to date.

  The daemon publishes the set whenever it changes (a registry load, a view
  that becomes stale or is in sync again) and clears it when it stops.  A
  view is in the set with the epoch of the definition that is maintained;
  a SYNCHRONOUS view that is not in the set with its current epoch (the
  daemon is not running, the view is stale, it was refreshed or created
  again and the daemon has not caught up yet) is not known to be current.
  Readers (the query rewrite plugin mview_rewrite) call
  mview_is_maintained() from any thread.

  @param views  (schema, view name, epoch) of every maintained view.
*/
struct Mview_maintained {
  std::string db;
  std::string name;
  uint64_t epoch{0};
};
void mview_set_maintained(const std::vector<Mview_maintained> &views);
/** The daemon stopped: no view is maintained any more. */
void mview_clear_maintained();
/**
  The daemon has applied the binary log up to the position (file name
  without directories, offset): every maintained view has every
  transaction that ends at or before it.
*/
void mview_set_applied_position(const std::string &file, uint64_t pos);
/** The position of mview_set_applied_position(); false if the daemon does
    not run. */
bool mview_get_applied_position(std::string *file, uint64_t *pos);
/** Is the view maintained, with this definition? */
bool mview_is_maintained(const std::string &db, const std::string &name,
                         uint64_t epoch);

/**
  Allow or forbid the session to change the contents of materialized view
  tables with ordinary DML.  User sessions are never allowed; the daemon's
  session and REFRESH MATERIALIZED VIEW turn this on.
*/
void mview_set_maintenance_mode(THD *thd, bool on);
bool mview_in_maintenance_mode(const THD *thd);

/**
  SQL text of a statement that fully rebuilds the view's contents,
  i.e. what REFRESH MATERIALIZED VIEW runs.  Convenience for the daemon,
  which may simply execute "REFRESH MATERIALIZED VIEW db.name" instead.
*/
std::string mview_refresh_statement(const Mview_def &def);

#endif  // SQL_MVIEW_INCLUDED
