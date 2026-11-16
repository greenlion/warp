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

#ifndef MVIEW_REWRITE_INCLUDED
#define MVIEW_REWRITE_INCLUDED

/**
  @file plugin/mview_rewrite/mview_rewrite.h

  The query rewrite plugin MVIEW_REWRITE: answers SELECT statements from
  the table-backed materialized views of the server (sql/mview.h).

  The matching follows J. Goldstein and P.-A. Larson, "Optimizing Queries
  Using Materialized Views: A Practical, Scalable Solution", SIGMOD 2001:

  - A query block and a view are select-project-join(-group-by)
    expressions.  Their conjuncts are split into column equalities (PE),
    range predicates on a column (PR) and residual predicates (PU).
  - Column equivalence classes are computed from PE.  A view contains all
    the rows the query needs if every view equivalence class is a subset of
    a query class (equijoin subsumption), every view range contains the
    range of the query class (range subsumption) and every residual
    conjunct of the view matches one of the query (residual subsumption),
    with column references compared through the query classes.
  - The rows are selected from the view with compensating predicates
    (equalities between view classes, tightened ranges, the residual
    conjuncts of the query the view lacks), and every expression the query
    needs is computed from the output columns of the view.
  - Tables of the view that the query does not reference must be joined
    through cardinality preserving joins (equijoins of non-NULL foreign
    keys to unique keys): the foreign key join graph is reduced by
    deleting nodes with no outgoing and exactly one incoming edge.
  - An aggregation query is computed from an aggregate view grouped on a
    superset of its grouping expressions, regrouping and rolling up the
    stored aggregate states (COUNT, SUM, MIN, MAX; AVG from SUM and COUNT)
    when the groups differ.
  - A filter tree of lattice indexes over view keys (hubs, residual
    predicates, source tables) narrows the views that are tested.

  Like the view-matching rule of a transformation based optimizer, which
  is invoked on every subexpression, a view may also replace a subset of
  the tables of a query block (a single-view substitute for an inner join
  subexpression); the other tables are joined to the view.  MySQL has no
  cost based choice among such rewrites at parse time, so the substitute
  is chosen by rules (see Candidate::score).

  Files:
    mview_rewrite_plugin.cc  the plugin, its variables, the registry of
                             analysed views and its loader thread
    mv_catalog.cc            table definitions from the data dictionary
    mv_expr.cc               expressions, literals, query blocks
    mv_match.cc              view matching and the rewritten statement
    mv_filter.cc             lattice index and filter tree
*/

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mysql/strings/m_ctype.h"
#include "sql/mview.h"

class Item;
class THD;
class Query_block;

namespace mv_rewrite {

/* ------------------------------------------------------------------ */
/* Table definitions                                                   */
/* ------------------------------------------------------------------ */

/** How the values of a column compare to literals and to other columns. */
enum class Type_class {
  NONE,  ///< not used for equivalences and ranges
  INT,
  DECIMAL,
  REAL,
  STRING,
  DATE,
  DATETIME,
  TIMESTAMP
};

struct Column_info {
  std::string name;
  Type_class tclass{Type_class::NONE};
  /// The collation of a string column, nullptr for other columns.
  const CHARSET_INFO *charset{nullptr};
  bool nullable{true};
  /// INVISIBLE: can be named, is not in SELECT *.
  bool invisible{false};
};

struct Table_info {
  std::string db;
  std::string name;
  std::string engine;
  /// A base table (not a view, not a system table).
  bool base_table{false};
  /// The table of a materialized view or its binlog position table.
  bool mview_table{false};
  /// Usable columns in table order (hidden columns are left out).
  std::vector<Column_info> columns;
  /// PRIMARY and UNIQUE keys without prefix parts: column names.
  std::vector<std::vector<std::string>> unique_keys;
  struct Foreign_key {
    std::vector<std::string> columns;
    std::string ref_db;
    std::string ref_table;
    std::vector<std::string> ref_columns;
  };
  std::vector<Foreign_key> foreign_keys;

  /** Index of the column, case insensitive; -1 if there is none. */
  int find(const std::string &column) const;
};

/**
  The definitions of the tables a statement or a registry load needs, read
  once from the data dictionary.  A table is read under a shared metadata
  lock that is released at once: a user session only tries the lock
  (never waits); the loader waits up to its lock_wait_timeout.
*/
class Catalog {
 public:
  Catalog(THD *thd, bool wait) : m_thd(thd), m_wait(wait) {}
  /** The table; nullptr if it does not exist or cannot be read now. */
  const Table_info *get(const std::string &db, const std::string &table);

 private:
  THD *m_thd;
  bool m_wait;
  std::map<std::pair<std::string, std::string>, std::unique_ptr<Table_info>>
      m_tables;
};

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

/** A literal, as SQL and as a value that ranges can compare. */
struct Literal {
  enum Kind { OTHER, NUMBER, STRING, TEMPORAL, NULL_VALUE };
  Kind kind{OTHER};
  /// The SQL of the literal.
  std::string text;
  /// NUMBER: decimal digits (exact) or a floating point number.
  bool exact{false};
  std::string number;
  /// STRING: the bytes and their character set; TEMPORAL: the date or
  /// datetime as it was written (between the quotes).
  std::string bytes;
  const CHARSET_INFO *charset{nullptr};
};

/**
  An expression of a query block, independent of the Item tree it was made
  from: column references, literals, and operators that print as a fixed
  text around their operands.  Two expressions match if their texts are
  equal and their column references are equivalent (the shallow matching
  of the paper, section 3.1.2).
*/
struct Expr {
  enum Kind { COLUMN, LITERAL, OP, AGG, STAR };
  Kind kind{OP};

  /// COLUMN: instance of the query block and column (as in the table).
  int inst{-1};
  std::string column;
  /// STAR: the qualifier of t.*, empty for *.
  std::string star_table;

  Literal lit;  ///< LITERAL

  /// OP, AGG: the printed expression is pieces[0] kid[slots[0]] pieces[1]
  /// ... kid[slots[n-1]] pieces[n].
  std::vector<std::string> pieces;
  std::vector<int> slots;
  std::vector<std::unique_ptr<Expr>> kids;
  /// OP: Item_func::Functype (or -1) and the NOT of BETWEEN / IN.
  int functype{-1};
  bool negated{false};
  /// AGG: Item_sum::Sumfunctype, DISTINCT, COUNT(*) (or of a constant).
  int sumfunc{-1};
  bool distinct{false};
  bool count_star{false};

  /// The text with every column printed as ?: used to compare quickly and
  /// as the key of the filter tree.
  std::string sig;
  /// Has an aggregate function.
  bool has_agg{false};

  /** The instances whose columns are referenced. */
  void instances(std::set<int> *out) const;
};

using Expr_ptr = std::unique_ptr<Expr>;

/** One table of a FROM clause. */
struct Instance {
  std::string db;
  std::string table;
  std::string alias;
  /// Only for the statement being rewritten.
  const Table_info *info{nullptr};
};

struct Select_item {
  Expr_ptr expr;
  /// The name of the column of the result.
  std::string name;
};

struct Order_item {
  Expr_ptr expr;     ///< nullptr for ORDER BY <number>
  int position{-1};  ///< ORDER BY <number>
  bool desc{false};
};

/**
  A query block with inner joins only: the statement to rewrite, or the
  definition of a view.
*/
struct Block {
  std::vector<Instance> insts;
  /// WHERE and the ON conditions, split at AND.
  std::vector<Expr_ptr> conjuncts;
  std::vector<Select_item> select;
  bool distinct{false};
  std::vector<Expr_ptr> group;
  std::vector<Expr_ptr> having;  ///< conjuncts
  std::vector<Order_item> order;
  std::string limit;  ///< " LIMIT ..." or empty
  bool has_agg{false};

  /** Aggregation: GROUP BY, aggregate functions or HAVING. */
  bool aggregated() const {
    return has_agg || !group.empty() || !having.empty();
  }
};

/**
  Make the block of a parsed query block.

  @param thd      Session the query block belongs to.
  @param qb       The query block, parsed (not resolved).
  @param catalog  Table definitions, for the columns of unqualified
                  references and of *.
  @param view     A view definition: every column reference is qualified
                  with an alias, the select list, GROUP BY and WHERE only.
  @param[out] block
  @param[out] why  Why the query block cannot be used.
  @retval false success
*/
bool make_block(THD *thd, Query_block *qb, Catalog *catalog, bool view,
                Block *block, std::string *why);

/** The SQL of an identifier, in backquotes. */
std::string quote(const std::string &name);

/** The SQL of a literal compared to a value of a column: <0, 0, >0;
    false if the comparison is not known (see Type_class). */
bool compare_literals(const Literal &a, const Literal &b,
                      const Column_info &column, int *result);
/** Can a range on the column be bounded by the literal? */
bool literal_fits(const Literal &lit, const Column_info &column);
/** Can an equality of the columns be used as an equivalence? */
bool equivalence_compatible(const Column_info &a, const Column_info &b);

/* ------------------------------------------------------------------ */
/* Views                                                               */
/* ------------------------------------------------------------------ */

/** What a select item of the block of a view is in the view table. */
struct View_output {
  enum Kind { PLAIN, GROUP, COUNT_STAR, STATE };
  Kind kind{PLAIN};
  std::string column;  ///< column of the view table
  int state{-1};       ///< STATE: index in View::states
};

/** A materialized view, analysed for matching. */
struct View {
  std::string db;
  std::string name;
  std::string engine;
  uint64_t epoch{0};
  Mview_refresh_mode refresh_mode{Mview_refresh_mode::MANUAL};
  bool is_aggregate{false};
  /// Mview_def::time_zone_dependent: the rows are the ones of UTC.
  bool time_zone_dependent{false};

  /// The definition as a query block over the base tables.  The select
  /// list is: a join view: the columns of the view (user and key columns);
  /// an aggregate view: the group expressions, COUNT(*), then one
  /// aggregate per state.  The GROUP BY of an aggregate view is its group
  /// expressions.
  Block block;
  std::vector<View_output> outputs;  ///< same order as block.select
  /// Aggregate views: the states, as in Mview_def::states.
  std::vector<Mview_agg_state::Kind> state_kinds;
  std::vector<int> state_count;  ///< SUM: output of its COUNT state

  /// Instances that a substitute must map to query tables (the hub of the
  /// paper, section 4.2.2), as a bit per instance.
  std::vector<bool> in_hub;
  /// Keys of the filter tree (interned strings).
  std::vector<int> hub_key;       ///< "db.table" of the hub
  std::vector<int> residual_key;  ///< signatures of residual conjuncts
  std::vector<int> source_key;    ///< "db.table" of every instance
  /// The view filters rows of its tables (ranges, residual conjuncts).
  bool filters{false};
};

/* ------------------------------------------------------------------ */
/* Filter tree                                                         */
/* ------------------------------------------------------------------ */

/**
  Lattice index (section 4.1): sets ordered by inclusion, with pointers to
  the minimal supersets and the maximal subsets of each set.
*/
class Lattice {
 public:
  /** Build from (key, payload) pairs; keys are sorted sets of ids. */
  void build(std::vector<std::pair<std::vector<int>, int>> entries);
  /** The payloads of the keys that are subsets of q. */
  void subsets_of(const std::vector<int> &q, std::vector<int> *out) const;
  /** The payloads of the keys that are supersets of q. */
  void supersets_of(const std::vector<int> &q, std::vector<int> *out) const;
  size_t size() const { return m_nodes.size(); }

 private:
  struct Node {
    std::vector<int> key;
    std::vector<int> payloads;
    std::vector<int> supersets;  ///< minimal supersets
    std::vector<int> subsets;    ///< maximal subsets
  };
  std::vector<Node> m_nodes;
  std::vector<int> m_roots;  ///< nodes without subsets
  std::vector<int> m_tops;   ///< nodes without supersets
};

/** The keys of a statement, to search the filter tree. */
struct Query_keys {
  std::vector<int> tables;     ///< "db.table" of the instances
  std::vector<int> residuals;  ///< signatures of the conjuncts
};

/**
  Filter tree (section 4.2): hubs (subsets of the tables of the query),
  then residual predicates (subsets of the conjuncts of the query); the
  aggregate views of a leaf are indexed by their source tables (supersets
  of the tables of the query: an aggregate view replaces all of them).
*/
class Filter_tree {
 public:
  void build(const std::vector<View> &views);
  void search(const Query_keys &keys, bool aggregate_views,
              std::vector<int> *views) const;

 private:
  struct Leaf {
    std::vector<int> join_views;
    Lattice aggregate_views;
  };
  struct Residual_level {
    Lattice lattice;  ///< payload: index in m_leaves
  };
  Lattice m_hubs;  ///< payload: index in m_residual_levels
  std::vector<Residual_level> m_residual_levels;
  std::vector<Leaf> m_leaves;
};

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

/** The analysed views of one version of the registry of the server. */
struct Registry {
  uint64_t version{0};
  std::vector<View> views;
  Filter_tree tree;
  std::map<std::string, int> strings;  ///< interned keys
  /// The collation the definitions were parsed with: string literals of
  /// a statement compare to those of the views only under the same one.
  const CHARSET_INFO *collation{nullptr};

  /** Interned id of a string; -1 if no view has it. */
  int id(const std::string &s) const {
    auto it = strings.find(s);
    return it == strings.end() ? -1 : it->second;
  }
  int intern(const std::string &s) {
    auto it = strings.emplace(s, static_cast<int>(strings.size()));
    return it.first->second;
  }
};

/**
  The parts of the analysis of a view that need the definitions of its
  tables: the hub, whether the view filters rows, and the signatures of
  its residual conjuncts (keys of the filter tree).

  @retval false success
  @retval true  the view cannot be used (why)
*/
bool analyse_view_tables(THD *thd, View *view, Catalog *catalog,
                         std::vector<std::string> *residuals,
                         std::string *why);

/** What the session asked for (system variables). */
struct Options {
  /// The WARP_MVIEW daemon has applied the last transaction the session
  /// committed: its SYNCHRONOUS views show the session its own changes.
  bool sync_current{true};
  bool use_manual{false};
  /// The time zone of the session is UTC.
  bool session_utc{true};
  bool use_foreign_keys{true};
  bool use_filter_tree{true};
  bool partial{true};
  bool trace{false};
};

/** Counters of one statement, added to the status variables. */
struct Counters {
  uint64_t candidates{0};  ///< views tested in full
};

/**
  Rewrite the statement of the session if a view can answer it.

  @param thd       Session; thd->lex holds the parsed statement.
  @param registry  The analysed views.
  @param options
  @param[out] sql      The new statement.
  @param[out] notes    Trace messages (options.trace).
  @param[out] counters
  @retval true  rewritten: sql is set
*/
bool rewrite_statement(THD *thd, const Registry &registry,
                       const Options &options, std::string *sql,
                       std::vector<std::string> *notes, Counters *counters);

}  // namespace mv_rewrite

#endif  // MVIEW_REWRITE_INCLUDED
