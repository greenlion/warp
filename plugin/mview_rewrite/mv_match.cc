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
  @file plugin/mview_rewrite/mv_match.cc

  View matching (section 3 of the paper) and the rewritten statement.

  A candidate is a view and a mapping of its table instances: each is
  mapped to a distinct instance of the query of the same base table, or is
  an extra table of the view that must be joined through cardinality
  preserving joins.  The query instances that are mapped (S) are replaced
  by the view; the others stay in the FROM clause (a partial substitute,
  the view-matching rule applied to the subexpression over S).

  Columns of the instances of the view, and of the query instances in S
  (through the mapping), share one numbering: the view space.  The
  equivalence classes of the view and of the query (the conjuncts of the
  query over S only) are union-find structures over it.
*/

#include "plugin/mview_rewrite/mview_rewrite.h"

#include <algorithm>
#include <functional>

#include "my_sqlcommand.h"
#include "mysql/strings/m_ctype.h"
#include "sql/parse_tree_nodes.h"  // PT_with_clause
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/opt_explain_format.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/system_variables.h"
#include "sql/table.h"

namespace mv_rewrite {

namespace {

/** The alias of the view when its name is the alias of a table that
    stays in the rewritten statement. */
const char *const FALLBACK_ALIAS = "mv$view";

std::string lower(const std::string &s) {
  std::string r = s;
  for (char &c : r)
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  return r;
}

bool same_ci(const std::string &a, const std::string &b) {
  return my_strcasecmp(system_charset_info, a.c_str(), b.c_str()) == 0;
}

/** Union-find over column numbers that grows on demand. */
class Classes {
 public:
  int find(int x) {
    grow(x);
    while (m_parent[x] != x) {
      m_parent[x] = m_parent[m_parent[x]];
      x = m_parent[x];
    }
    return x;
  }
  void merge(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) m_parent[std::max(a, b)] = std::min(a, b);
  }
  bool same(int a, int b) { return find(a) == find(b); }

 private:
  void grow(int x) {
    while (static_cast<int>(m_parent.size()) <= x)
      m_parent.push_back(static_cast<int>(m_parent.size()));
  }
  std::vector<int> m_parent;
};

struct Bound {
  const Literal *lit{nullptr};
  bool inclusive{true};
  bool set() const { return lit != nullptr; }
};

struct Range {
  Bound lo, hi;
  const Column_info *column{nullptr};
};

/** A classified conjunct. */
struct Pred {
  enum Kind { EQ, RANGE, RESIDUAL };
  Kind kind{RESIDUAL};
  int a{-1}, b{-1};  ///< EQ: columns; RANGE: a
  Bound lo, hi;      ///< RANGE
  const Column_info *column{nullptr};
  const Expr *expr{nullptr};
};

/** The comparison of two bounds of the same side. */
int compare_bounds(const Bound &a, const Bound &b, const Column_info &column,
                   bool lower, bool *ok) {
  int r = 0;
  if (!compare_literals(*a.lit, *b.lit, column, &r)) {
    *ok = false;
    return 0;
  }
  *ok = true;
  if (r != 0) return r;
  if (a.inclusive == b.inclusive) return 0;
  /* the exclusive bound is the tighter one */
  if (lower) return a.inclusive ? -1 : 1;
  return a.inclusive ? 1 : -1;
}

/** Tighten a range with a bound; false if the literals do not compare. */
bool tighten(Range *range, const Bound &bound, bool lower) {
  Bound &cur = lower ? range->lo : range->hi;
  if (!cur.set()) {
    cur = bound;
    return true;
  }
  bool ok;
  const int r = compare_bounds(bound, cur, *range->column, lower, &ok);
  if (!ok) return false;
  /* lower: larger is tighter; upper: smaller is tighter */
  if ((lower && r > 0) || (!lower && r < 0)) cur = bound;
  return true;
}

/** Is the range a contained in the range b (tighter or equal)? */
bool contained(const Range &a, const Range &b, const Column_info &column,
               bool *ok) {
  *ok = true;
  if (b.lo.set()) {
    if (!a.lo.set()) return false;
    const int r = compare_bounds(a.lo, b.lo, column, true, ok);
    if (!*ok || r < 0) return false;
  }
  if (b.hi.set()) {
    if (!a.hi.set()) return false;
    const int r = compare_bounds(a.hi, b.hi, column, false, ok);
    if (!*ok || r > 0) return false;
  }
  return true;
}

/** How the aggregates of a query are taken from an aggregate view. */
enum class Agg_mode {
  NONE,    ///< a join view, or no aggregation
  EXACT,   ///< the groups of the query are the groups of the view
  ROLLUP,  ///< the groups of the view are regrouped
  DISTINCT ///< SELECT DISTINCT over the groups of the view
};

/** A rewrite that was found, and how good it is. */
struct Candidate {
  std::string sql;
  /// Lexicographic: tables replaced, aggregation done by the view, groups
  /// taken as they are, fewer extra tables, fewer compensations.
  std::vector<long> score;
};

class Statement;

/** Matching of one view with one mapping. */
class Matcher {
 public:
  Matcher(Statement *st, const View &view, std::vector<int> v2q);
  /** @retval true a rewrite was made (candidate is set) */
  bool run(Candidate *candidate);
  /**
    For the registry: the hub of the view (the instances that are left
    when every table that can be is eliminated), whether it filters rows,
    and the signatures of its residual conjuncts.
  */
  bool analyse(std::vector<bool> *in_hub, bool *filters,
               std::vector<std::string> *residuals);
  std::string why;

 private:
  bool fail(const std::string &reason) {
    if (why.empty()) why = reason;
    return false;
  }
  int col_id(int vinst, const std::string &column);
  const Column_info *column_info(int id) const;
  int query_col(const Expr &e);  ///< view space id of a query column; -1
  bool classify(const Expr &e, bool query, Pred *pred);
  bool match(const Expr &q, const Expr &v);
  bool match_qq(const Expr &a, const Expr &b);
  bool output_usable(size_t output);
  /** The view column for a view space column (through the query or the
      view classes); empty if none. */
  std::string simple_output(int id, bool view_classes);
  std::string view_column(const std::string &column) const {
    return quote(m_alias) + "." + quote(column);
  }
  bool load_tables();
  bool classify_view();
  bool eliminate_extra_tables();
  bool compensate(std::vector<std::string> *where);

  /* Translation of query expressions to SQL over the view. */
  bool translate(const Expr &e, bool in_agg, std::string *out);
  bool translate_where(const Expr &e, std::string *out);
  bool translate_plain(const Expr &e, std::string *out);
  bool translate_group(const Expr &e, std::string *out);
  bool translate_agg(const Expr &e, std::string *out);
  bool translate_kids(const Expr &e, bool in_agg, std::string *out);
  bool group_value(const Expr &e, std::string *out);
  bool print_outer(const Expr &e, std::string *out);
  bool uses_view(const Expr &e) const;
  bool decide_mode();
  bool build_sql(const std::vector<std::string> &where, Candidate *c);

  Statement *m_st;
  const Block &m_q;
  const View &m_v;
  std::vector<int> m_v2q;
  std::vector<int> m_q2v;
  /// The alias of the view in the rewritten statement: its name, which
  /// EXPLAIN shows.
  std::string m_alias;
  std::vector<const Table_info *> m_vinfo;
  const Table_info *m_view_table{nullptr};
  bool m_warp_view{false};

  std::map<std::pair<int, std::string>, int> m_ids;
  std::vector<std::pair<int, std::string>> m_names;
  Classes m_qc, m_vc;
  std::map<int, std::vector<int>> m_qmembers;  ///< query class -> columns
  std::map<int, std::vector<int>> m_vmembers;

  std::vector<Pred> m_local;     ///< classified conjuncts of the query
  std::vector<bool> m_is_local;  ///< per conjunct of the query
  std::vector<Pred> m_view_preds;
  std::vector<std::pair<int, int>> m_fk_equalities;
  /// Instances that are left after the elimination of extra tables.
  std::vector<bool> m_alive;
  /// The registry asks for the hub: every instance may be eliminated.
  bool m_hub_mode{false};

  std::vector<int> m_out_ok;  ///< -1 unknown, 0 no, 1 yes
  /// view space column -> outputs that are that column
  std::map<int, std::vector<size_t>> m_simple_outputs;

  Agg_mode m_mode{Agg_mode::NONE};
  bool m_grouped_query{false};
  int m_compensations{0};
};

/** The statement being rewritten and what all candidates share. */
class Statement {
 public:
  Statement(THD *thd, const Options &options, Block *block, Catalog *catalog,
            std::vector<std::string> *notes, Counters *counters)
      : thd(thd),
        options(options),
        q(*block),
        catalog(catalog),
        notes(notes),
        counters(counters) {}

  THD *thd;
  const Options &options;
  const Block &q;
  Catalog *catalog;
  std::vector<std::string> *notes;
  Counters *counters;
  bool only_full_group_by{false};

  void note(const View &v, const std::string &text) {
    if (options.trace)
      notes->push_back("view " + quote(v.db) + "." + quote(v.name) + ": " +
                       text);
  }
};

Matcher::Matcher(Statement *st, const View &view, std::vector<int> v2q)
    : m_st(st), m_q(st->q), m_v(view), m_v2q(std::move(v2q)) {
  m_q2v.assign(m_q.insts.size(), -1);
  for (size_t j = 0; j < m_v2q.size(); j++)
    if (m_v2q[j] >= 0) m_q2v[m_v2q[j]] = static_cast<int>(j);
  /* the aliases of the tables that stay must not be the alias of the view */
  auto taken = [&](const std::string &alias) {
    for (size_t i = 0; i < m_q.insts.size(); i++)
      if (m_q2v[i] < 0 && same_ci(m_q.insts[i].alias, alias)) return true;
    return false;
  };
  if (!taken(m_v.name))
    m_alias = m_v.name;
  else if (!taken(FALLBACK_ALIAS))
    m_alias = FALLBACK_ALIAS;
}

int Matcher::col_id(int vinst, const std::string &column) {
  const auto key = std::make_pair(vinst, lower(column));
  auto it = m_ids.find(key);
  if (it != m_ids.end()) return it->second;
  const int id = static_cast<int>(m_names.size());
  m_ids.emplace(key, id);
  m_names.emplace_back(vinst, column);
  return id;
}

const Column_info *Matcher::column_info(int id) const {
  const auto &n = m_names[id];
  const Table_info *info = m_vinfo[n.first];
  const int c = info->find(n.second);
  return c < 0 ? nullptr : &info->columns[c];
}

int Matcher::query_col(const Expr &e) {
  if (e.kind != Expr::COLUMN || m_q2v[e.inst] < 0) return -1;
  return col_id(m_q2v[e.inst], e.column);
}

/** The literal of an operand, nullptr if it is not one. */
const Literal *literal_of(const Expr &e) {
  return e.kind == Expr::LITERAL ? &e.lit : nullptr;
}

bool Matcher::classify(const Expr &e, bool query, Pred *pred) {
  pred->expr = &e;
  pred->kind = Pred::RESIDUAL;
  auto column = [&](const Expr &x) -> int {
    if (x.kind != Expr::COLUMN) return -1;
    return query ? query_col(x) : col_id(x.inst, x.column);
  };
  if (e.kind != Expr::OP) return true;
  const size_t n = e.kids.size();
  const int ft = e.functype;

  if ((ft == Item_func::EQ_FUNC || ft == Item_func::LT_FUNC ||
       ft == Item_func::LE_FUNC || ft == Item_func::GE_FUNC ||
       ft == Item_func::GT_FUNC) &&
      n == 2) {
    const int c0 = column(*e.kids[0]);
    const int c1 = column(*e.kids[1]);
    if (ft == Item_func::EQ_FUNC && c0 >= 0 && c1 >= 0) {
      const Column_info *i0 = column_info(c0);
      const Column_info *i1 = column_info(c1);
      if (i0 == nullptr || i1 == nullptr) return false;
      if (equivalence_compatible(*i0, *i1)) {
        pred->kind = Pred::EQ;
        pred->a = c0;
        pred->b = c1;
      }
      return true;
    }
    int c = c0;
    const Literal *lit = literal_of(*e.kids[1]);
    int op = ft;
    if (c < 0 || lit == nullptr) {
      c = c1;
      lit = literal_of(*e.kids[0]);
      /* 5 < x is x > 5 */
      if (ft == Item_func::LT_FUNC) op = Item_func::GT_FUNC;
      if (ft == Item_func::LE_FUNC) op = Item_func::GE_FUNC;
      if (ft == Item_func::GT_FUNC) op = Item_func::LT_FUNC;
      if (ft == Item_func::GE_FUNC) op = Item_func::LE_FUNC;
    }
    if (c < 0 || lit == nullptr) return true;
    const Column_info *info = column_info(c);
    if (info == nullptr) return false;
    if (!literal_fits(*lit, *info)) return true;
    pred->kind = Pred::RANGE;
    pred->a = c;
    pred->column = info;
    if (op == Item_func::EQ_FUNC || op == Item_func::GE_FUNC ||
        op == Item_func::GT_FUNC)
      pred->lo = {lit, op != Item_func::GT_FUNC};
    if (op == Item_func::EQ_FUNC || op == Item_func::LE_FUNC ||
        op == Item_func::LT_FUNC)
      pred->hi = {lit, op != Item_func::LT_FUNC};
    return true;
  }
  if (ft == Item_func::BETWEEN && !e.negated && n == 3) {
    const int c = column(*e.kids[0]);
    const Literal *lo = literal_of(*e.kids[1]);
    const Literal *hi = literal_of(*e.kids[2]);
    if (c < 0 || lo == nullptr || hi == nullptr) return true;
    const Column_info *info = column_info(c);
    if (info == nullptr) return false;
    if (!literal_fits(*lo, *info) || !literal_fits(*hi, *info)) return true;
    pred->kind = Pred::RANGE;
    pred->a = c;
    pred->column = info;
    pred->lo = {lo, true};
    pred->hi = {hi, true};
    return true;
  }
  if (ft == Item_func::IN_FUNC && !e.negated && n == 2) {
    const int c = column(*e.kids[0]);
    const Literal *lit = literal_of(*e.kids[1]);
    if (c < 0 || lit == nullptr) return true;
    const Column_info *info = column_info(c);
    if (info == nullptr) return false;
    if (!literal_fits(*lit, *info)) return true;
    pred->kind = Pred::RANGE;
    pred->a = c;
    pred->column = info;
    pred->lo = {lit, true};
    pred->hi = {lit, true};
  }
  return true;
}

/** Structural match of a query expression (columns of S) and a view
    expression, with column references compared through the query
    classes. */
bool Matcher::match(const Expr &q, const Expr &v) {
  if (q.kind != v.kind) return false;
  switch (q.kind) {
    case Expr::COLUMN: {
      const int a = query_col(q);
      if (a < 0) return false;
      return m_qc.same(a, col_id(v.inst, v.column));
    }
    case Expr::LITERAL:
      return q.lit.text == v.lit.text;
    case Expr::OP:
    case Expr::AGG:
      if (q.sig != v.sig || q.pieces != v.pieces || q.slots != v.slots ||
          q.kids.size() != v.kids.size())
        return false;
      for (size_t i = 0; i < q.kids.size(); i++)
        if (!match(*q.kids[i], *v.kids[i])) return false;
      return true;
    default:
      return false;
  }
}

/** Match of two query expressions (columns of S through the query
    classes, other columns by identity). */
bool Matcher::match_qq(const Expr &a, const Expr &b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case Expr::COLUMN: {
      const int ia = query_col(a);
      const int ib = query_col(b);
      if (ia >= 0 && ib >= 0) return m_qc.same(ia, ib);
      if (ia >= 0 || ib >= 0) return false;
      return a.inst == b.inst && same_ci(a.column, b.column);
    }
    case Expr::LITERAL:
      return a.lit.text == b.lit.text;
    case Expr::OP:
    case Expr::AGG:
      if (a.sig != b.sig || a.pieces != b.pieces || a.slots != b.slots ||
          a.kids.size() != b.kids.size())
        return false;
      for (size_t i = 0; i < a.kids.size(); i++)
        if (!match_qq(*a.kids[i], *b.kids[i])) return false;
      return true;
    default:
      return false;
  }
}

/**
  Can an output of the view stand for its expression?  A string column of
  a WARP view may have a binary collation instead of the one of the
  expression (WARP stores only binary and case sensitive collations):
  comparisons, grouping and ordering on it would differ.
*/
bool Matcher::output_usable(size_t output) {
  if (m_out_ok[output] >= 0) return m_out_ok[output] == 1;
  bool ok = false;
  const int c = m_view_table->find(m_v.outputs[output].column);
  if (c >= 0) {
    const Column_info &vc = m_view_table->columns[c];
    if (vc.tclass != Type_class::STRING) {
      ok = true;
    } else {
      const Expr &e = *m_v.block.select[output].expr;
      const Expr *base = &e;
      if (e.kind == Expr::AGG && e.kids.size() == 1 &&
          e.kids[0]->kind == Expr::COLUMN)
        base = e.kids[0].get();
      if (base->kind == Expr::COLUMN) {
        const Column_info *bc = column_info(col_id(base->inst, base->column));
        ok = bc != nullptr && bc->charset == vc.charset;
      } else {
        ok = !m_warp_view || vc.charset == &my_charset_bin;
      }
    }
  }
  m_out_ok[output] = ok ? 1 : 0;
  return ok;
}

std::string Matcher::simple_output(int id, bool view_classes) {
  Classes &classes = view_classes ? m_vc : m_qc;
  const int root = classes.find(id);
  for (const auto &entry : m_simple_outputs) {
    if (classes.find(entry.first) != root) continue;
    for (size_t output : entry.second)
      if (output_usable(output)) return view_column(m_v.outputs[output].column);
  }
  return "";
}

/**
  Extra tables of the view (section 3.2): build the foreign key join graph
  and delete nodes with no outgoing and exactly one incoming edge.  The
  join conditions of the deleted edges become query equivalences.
*/
bool Matcher::eliminate_extra_tables() {
  const int n = static_cast<int>(m_v2q.size());
  m_alive.assign(n, true);
  bool any_extra = false;
  for (int q : m_v2q)
    if (q < 0) any_extra = true;
  if (!any_extra) return true;
  if (!m_st->options.use_foreign_keys)
    return m_hub_mode || fail("the view has tables the query does not have");

  /* An instance with a column that is in a range or residual conjunct of
     the view and equal to no other column cannot be eliminated: the query
     would need the same conjunct, on a column of that table (section
     4.2.2). */
  std::set<int> joined;
  for (const Pred &p : m_view_preds)
    if (p.kind == Pred::EQ) {
      joined.insert(p.a);
      joined.insert(p.b);
    }
  std::vector<bool> pinned(n, false);
  for (const Expr_ptr &c : m_v.block.conjuncts) {
    std::function<void(const Expr &)> walk = [&](const Expr &e) {
      if (e.kind == Expr::COLUMN &&
          joined.count(col_id(e.inst, e.column)) == 0)
        pinned[e.inst] = true;
      for (const Expr_ptr &kid : e.kids) walk(*kid);
    };
    bool is_eq = false;
    for (const Pred &p : m_view_preds)
      if (p.expr == c.get() && p.kind == Pred::EQ) is_eq = true;
    if (!is_eq) walk(*c);
  }

  struct Edge {
    int from, to;
    std::vector<std::pair<int, int>> pairs;
  };
  std::vector<Edge> edges;

  /* columns the query rejects NULL for: in a range or equal to another */
  auto null_rejected = [&](int id) {
    for (const Pred &p : m_local) {
      if (p.kind == Pred::RANGE && m_qc.same(p.a, id)) return true;
      if (p.kind == Pred::EQ && (m_qc.same(p.a, id) || m_qc.same(p.b, id)))
        return true;
    }
    return false;
  };

  for (int i = 0; i < n; i++) {
    const Table_info *from = m_vinfo[i];
    for (const Table_info::Foreign_key &fk : from->foreign_keys) {
      for (int j = 0; j < n; j++) {
        if (j == i) continue;
        const Table_info *to = m_vinfo[j];
        if (fk.ref_db != to->db || fk.ref_table != to->name) continue;
        /* the referenced columns are a unique key */
        bool unique = false;
        for (const auto &key : to->unique_keys) {
          if (key.size() != fk.ref_columns.size()) continue;
          bool all = true;
          for (const std::string &k : key) {
            bool found = false;
            for (const std::string &r : fk.ref_columns)
              if (same_ci(k, r)) found = true;
            if (!found) all = false;
          }
          if (all) unique = true;
        }
        if (!unique) continue;
        Edge edge{i, j, {}};
        bool ok = true;
        for (size_t k = 0; k < fk.columns.size() && ok; k++) {
          const int f = from->find(fk.columns[k]);
          const int c = to->find(fk.ref_columns[k]);
          if (f < 0 || c < 0) {
            ok = false;
            break;
          }
          const int fid = col_id(i, from->columns[f].name);
          const int cid = col_id(j, to->columns[c].name);
          /* the view joins them (directly or transitively) */
          if (!m_vc.same(fid, cid)) ok = false;
          /* every row has a non-NULL foreign key, or the query does not
             want the rows with a NULL */
          if (from->columns[f].nullable &&
              !(m_v2q[i] >= 0 && null_rejected(fid)))
            ok = false;
          edge.pairs.emplace_back(fid, cid);
        }
        if (ok) edges.push_back(std::move(edge));
      }
    }
  }

  std::vector<bool> &alive = m_alive;
  bool progress = true;
  while (progress) {
    progress = false;
    for (int j = 0; j < n; j++) {
      if (!alive[j] || m_v2q[j] >= 0 || pinned[j]) continue;
      int outgoing = 0, incoming = 0;
      const Edge *in = nullptr;
      for (const Edge &e : edges) {
        if (!alive[e.from] || !alive[e.to]) continue;
        if (e.from == j) outgoing++;
        if (e.to == j) {
          incoming++;
          in = &e;
        }
      }
      if (outgoing == 0 && incoming == 1) {
        alive[j] = false;
        for (const auto &p : in->pairs) m_fk_equalities.push_back(p);
        progress = true;
      }
    }
  }
  if (m_hub_mode) return true;
  for (int j = 0; j < n; j++)
    if (alive[j] && m_v2q[j] < 0)
      return fail("table " + m_v.block.insts[j].alias +
                  " of the view is not joined by a foreign key to a unique "
                  "key (not NULL)");
  return true;
}

/**
  The subsumption tests (section 3.1.2) and the compensating predicates
  (section 3.1.3).
*/
bool Matcher::compensate(std::vector<std::string> *where) {
  /* equijoin subsumption: every view class is inside a query class */
  for (const Pred &p : m_view_preds) {
    if (p.kind != Pred::EQ) continue;
    if (!m_qc.same(p.a, p.b))
      return fail("the view has a column equality the query does not have");
  }

  /* compensating equalities between the view classes of a query class */
  std::map<int, std::vector<int>> view_classes_of;  // query root -> view roots
  for (int id = 0; id < static_cast<int>(m_names.size()); id++) {
    auto &roots = view_classes_of[m_qc.find(id)];
    const int vr = m_vc.find(id);
    if (std::find(roots.begin(), roots.end(), vr) == roots.end())
      roots.push_back(vr);
  }
  for (const auto &entry : view_classes_of) {
    if (entry.second.size() < 2) continue;
    std::string first;
    for (int vr : entry.second) {
      const std::string col = simple_output(vr, true);
      if (col.empty())
        return fail("a column of a compensating equality is not in the view");
      if (first.empty()) {
        first = col;
      } else {
        where->push_back("(" + first + " = " + col + ")");
        m_compensations++;
      }
    }
  }

  /* ranges of the query and of the view */
  std::map<int, Range> qranges, vranges;  // by query root / view root
  for (const Pred &p : m_local) {
    if (p.kind != Pred::RANGE) continue;
    Range &r = qranges[m_qc.find(p.a)];
    if (r.column == nullptr) r.column = p.column;
    if ((p.lo.set() && !tighten(&r, p.lo, true)) ||
        (p.hi.set() && !tighten(&r, p.hi, false)))
      return fail("ranges of the query cannot be compared");
  }
  for (const Pred &p : m_view_preds) {
    if (p.kind != Pred::RANGE) continue;
    Range &r = vranges[m_vc.find(p.a)];
    if (r.column == nullptr) r.column = p.column;
    if ((p.lo.set() && !tighten(&r, p.lo, true)) ||
        (p.hi.set() && !tighten(&r, p.hi, false)))
      return fail("ranges of the view cannot be compared");
  }
  /* range subsumption, and the view range of each query class */
  std::map<int, Range> effective;  // query root -> tightest view range
  for (const auto &entry : vranges) {
    const int qroot = m_qc.find(entry.first);
    const Range &vr = entry.second;
    Range qr;
    auto it = qranges.find(qroot);
    if (it != qranges.end()) qr = it->second;
    bool ok;
    if (!contained(qr, vr, *vr.column, &ok) || !ok)
      return fail("a range of the view does not contain the range of the "
                  "query");
    Range &eff = effective[qroot];
    if (eff.column == nullptr) eff.column = vr.column;
    if ((vr.lo.set() && !tighten(&eff, vr.lo, true)) ||
        (vr.hi.set() && !tighten(&eff, vr.hi, false)))
      return fail("ranges of the view cannot be compared");
  }
  /* compensating ranges */
  for (const auto &entry : qranges) {
    const Range &qr = entry.second;
    Range eff;
    auto it = effective.find(entry.first);
    if (it != effective.end()) eff = it->second;
    bool need_lo = false, need_hi = false, ok = true;
    if (qr.lo.set())
      need_lo = !eff.lo.set() ||
                compare_bounds(qr.lo, eff.lo, *qr.column, true, &ok) > 0;
    if (!ok) return fail("ranges cannot be compared");
    if (qr.hi.set())
      need_hi = !eff.hi.set() ||
                compare_bounds(qr.hi, eff.hi, *qr.column, false, &ok) < 0;
    if (!ok) return fail("ranges cannot be compared");
    if (!need_lo && !need_hi) continue;
    const std::string col = simple_output(entry.first, false);
    if (col.empty())
      return fail("a column of a compensating range is not in the view");
    int r = 1;
    if (need_lo && need_hi && qr.lo.inclusive && qr.hi.inclusive &&
        compare_literals(*qr.lo.lit, *qr.hi.lit, *qr.column, &r) && r == 0) {
      where->push_back("(" + col + " = " + qr.lo.lit->text + ")");
      m_compensations++;
      continue;
    }
    if (need_lo) {
      where->push_back("(" + col + (qr.lo.inclusive ? " >= " : " > ") +
                       qr.lo.lit->text + ")");
      m_compensations++;
    }
    if (need_hi) {
      where->push_back("(" + col + (qr.hi.inclusive ? " <= " : " < ") +
                       qr.hi.lit->text + ")");
      m_compensations++;
    }
  }

  /* residual subsumption, and the residual conjuncts the view lacks */
  std::vector<bool> matched(m_local.size(), false);
  for (const Pred &vp : m_view_preds) {
    if (vp.kind != Pred::RESIDUAL) continue;
    bool found = false;
    for (size_t i = 0; i < m_local.size() && !found; i++) {
      if (m_local[i].kind != Pred::RESIDUAL) continue;
      if (match(*m_local[i].expr, *vp.expr)) {
        matched[i] = true;
        found = true;
      }
    }
    if (!found)
      return fail("the view has a condition the query does not have");
  }
  for (size_t i = 0; i < m_local.size(); i++) {
    if (m_local[i].kind != Pred::RESIDUAL || matched[i]) continue;
    std::string sql;
    if (!translate_where(*m_local[i].expr, &sql))
      return fail("a compensating condition cannot be computed from the "
                  "view");
    where->push_back("(" + sql + ")");
    m_compensations++;
  }
  return true;
}

bool Matcher::uses_view(const Expr &e) const {
  if (e.kind == Expr::COLUMN) return m_q2v[e.inst] >= 0;
  for (const Expr_ptr &kid : e.kids)
    if (uses_view(*kid)) return true;
  return false;
}

/** An expression over the tables that stay. */
bool Matcher::print_outer(const Expr &e, std::string *out) {
  switch (e.kind) {
    case Expr::COLUMN:
      if (m_q2v[e.inst] >= 0) return false;
      *out = quote(m_q.insts[e.inst].alias) + "." + quote(e.column);
      return true;
    case Expr::LITERAL:
      *out = e.lit.text;
      return true;
    case Expr::OP:
    case Expr::AGG: {
      std::string s = e.pieces[0];
      for (size_t i = 0; i < e.slots.size(); i++) {
        std::string kid;
        if (!print_outer(*e.kids[e.slots[i]], &kid)) return false;
        s += kid;
        s += e.pieces[i + 1];
      }
      *out = std::move(s);
      return true;
    }
    default:
      return false;
  }
}

bool Matcher::translate_kids(const Expr &e, bool in_agg, std::string *out) {
  std::string s = e.pieces[0];
  for (size_t i = 0; i < e.slots.size(); i++) {
    std::string kid;
    if (!translate(*e.kids[e.slots[i]], in_agg, &kid)) return false;
    s += kid;
    s += e.pieces[i + 1];
  }
  *out = std::move(s);
  return true;
}

/** A query expression over a join view (and the tables that stay). */
bool Matcher::translate_plain(const Expr &e, std::string *out) {
  if (!uses_view(e)) return print_outer(e, out);
  if (e.kind != Expr::LITERAL) {
    for (size_t i = 0; i < m_v.outputs.size(); i++) {
      if (match(e, *m_v.block.select[i].expr) && output_usable(i)) {
        *out = view_column(m_v.outputs[i].column);
        return true;
      }
    }
  }
  switch (e.kind) {
    case Expr::COLUMN:
      *out = simple_output(query_col(e), false);
      return !out->empty();
    case Expr::LITERAL:
      *out = e.lit.text;
      return true;
    case Expr::OP:
    case Expr::AGG: {
      std::string s = e.pieces[0];
      for (size_t i = 0; i < e.slots.size(); i++) {
        std::string kid;
        if (!translate_plain(*e.kids[e.slots[i]], &kid)) return false;
        s += kid;
        s += e.pieces[i + 1];
      }
      *out = std::move(s);
      return true;
    }
    default:
      return false;
  }
}

/** A query expression outside of aggregate functions, over the group
    columns of an aggregate view. */
bool Matcher::group_value(const Expr &e, std::string *out) {
  if (!uses_view(e)) return print_outer(e, out);
  if (e.kind != Expr::LITERAL) {
    for (size_t i = 0; i < m_v.outputs.size(); i++) {
      if (m_v.outputs[i].kind != View_output::GROUP) continue;
      if (match(e, *m_v.block.select[i].expr) && output_usable(i)) {
        *out = view_column(m_v.outputs[i].column);
        return true;
      }
    }
  }
  switch (e.kind) {
    case Expr::COLUMN:
      *out = simple_output(query_col(e), false);
      return !out->empty();
    case Expr::LITERAL:
      *out = e.lit.text;
      return true;
    case Expr::OP: {
      std::string s = e.pieces[0];
      for (size_t i = 0; i < e.slots.size(); i++) {
        std::string kid;
        if (!group_value(*e.kids[e.slots[i]], &kid)) return false;
        s += kid;
        s += e.pieces[i + 1];
      }
      *out = std::move(s);
      return true;
    }
    default:
      return false;
  }
}

/** An expression of an aggregate view outside of aggregate functions. */
bool Matcher::translate_group(const Expr &e, std::string *out) {
  if (e.kind == Expr::AGG) return translate_agg(e, out);
  if (e.has_agg) {
    if (e.kind != Expr::OP) return false;
    return translate_kids(e, false, out);
  }
  return group_value(e, out);
}

/** An aggregate function from the states or the groups of the view. */
bool Matcher::translate_agg(const Expr &e, std::string *out) {
  const bool exact = m_mode == Agg_mode::EXACT;
  const Expr *arg = e.kids.size() == 1 ? e.kids[0].get() : nullptr;

  auto state_of = [&](Mview_agg_state::Kind kind) -> int {
    if (arg == nullptr) return -1;
    for (size_t i = 0; i < m_v.outputs.size(); i++) {
      const View_output &o = m_v.outputs[i];
      if (o.kind != View_output::STATE || m_v.state_kinds[o.state] != kind)
        continue;
      const Expr &se = *m_v.block.select[i].expr;
      if (se.kids.size() == 1 && match(*arg, *se.kids[0]) &&
          output_usable(i))
        return static_cast<int>(i);
    }
    return -1;
  };
  auto col = [&](int output) { return view_column(m_v.outputs[output].column); };

  if (!e.distinct) {
    switch (e.sumfunc) {
      case Item_sum::COUNT_FUNC: {
        std::string c;
        if (e.count_star) {
          for (size_t i = 0; i < m_v.outputs.size(); i++)
            if (m_v.outputs[i].kind == View_output::COUNT_STAR)
              c = view_column(m_v.outputs[i].column);
        } else {
          const int s = state_of(Mview_agg_state::COUNT);
          if (s >= 0) c = col(s);
        }
        if (c.empty()) break;
        *out = exact ? c : "CAST(COALESCE(SUM(" + c + "),0) AS SIGNED)";
        return true;
      }
      case Item_sum::SUM_FUNC: {
        const int s = state_of(Mview_agg_state::SUM);
        if (s < 0) break;
        *out = exact ? col(s) : "SUM(" + col(s) + ")";
        return true;
      }
      case Item_sum::AVG_FUNC: {
        const int s = state_of(Mview_agg_state::SUM);
        if (s < 0) break;
        const int c = m_v.state_count[m_v.outputs[s].state];
        if (c < 0) break;
        *out = exact ? "(" + col(s) + " / NULLIF(" + col(c) + ",0))"
                     : "(SUM(" + col(s) + ") / NULLIF(SUM(" + col(c) + "),0))";
        return true;
      }
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC: {
        const bool is_min = e.sumfunc == Item_sum::MIN_FUNC;
        const int s =
            state_of(is_min ? Mview_agg_state::MIN : Mview_agg_state::MAX);
        if (s < 0) break;
        *out = exact ? col(s) : (is_min ? "MIN(" : "MAX(") + col(s) + ")";
        return true;
      }
      default:
        break;
    }
  }

  /* a stored aggregate that is recomputed: only as it is */
  if (exact) {
    for (size_t i = 0; i < m_v.outputs.size(); i++) {
      const View_output &o = m_v.outputs[i];
      if (o.kind != View_output::STATE) continue;
      if (match(e, *m_v.block.select[i].expr) && output_usable(i)) {
        *out = view_column(o.column);
        return true;
      }
    }
  }

  /* aggregates that give the same value over the groups of the view as
     over their rows, of group expressions */
  const bool bit_and_or = e.sumfunc == Item_sum::SUM_BIT_FUNC &&
                          (e.pieces[0].compare(0, 8, "bit_and(") == 0 ||
                           e.pieces[0].compare(0, 7, "bit_or(") == 0);
  const bool idempotent = e.sumfunc == Item_sum::MIN_FUNC ||
                          e.sumfunc == Item_sum::MAX_FUNC || e.distinct ||
                          bit_and_or;
  if (!idempotent) return false;
  if (exact) {
    /* the value of the group: MIN and MAX of a group expression */
    if ((e.sumfunc == Item_sum::MIN_FUNC || e.sumfunc == Item_sum::MAX_FUNC) &&
        arg != nullptr && !e.distinct)
      return group_value(*arg, out);
    return false;
  }
  std::string s = e.pieces[0];
  for (size_t i = 0; i < e.slots.size(); i++) {
    std::string kid;
    if (!group_value(*e.kids[e.slots[i]], &kid)) return false;
    s += kid;
    s += e.pieces[i + 1];
  }
  *out = std::move(s);
  return true;
}

/**
  A query expression as SQL over the view.  In a grouped query, a column
  outside of aggregate functions must be in a GROUP BY expression of the
  query (the view does not tell which columns depend on which).
*/
bool Matcher::translate(const Expr &e, bool in_agg, std::string *out) {
  if (m_grouped_query && !in_agg && !e.has_agg &&
      m_mode != Agg_mode::EXACT && e.kind != Expr::LITERAL) {
    for (const Expr_ptr &g : m_q.group) {
      if (match_qq(e, *g)) {
        return m_mode == Agg_mode::NONE ? translate_plain(*g, out)
                                        : group_value(*g, out);
      }
    }
    if (e.kind == Expr::COLUMN) {
      if (m_st->only_full_group_by) return false;
      std::string inner;
      const bool ok = m_mode == Agg_mode::NONE ? translate_plain(e, &inner)
                                              : group_value(e, &inner);
      if (!ok) return false;
      *out = "ANY_VALUE(" + inner + ")";
      return true;
    }
    if (e.kind == Expr::OP) return translate_kids(e, in_agg, out);
  }
  if (m_mode == Agg_mode::NONE) {
    if (e.kind == Expr::AGG) {
      if (!uses_view(e)) return print_outer(e, out);
      return translate_kids(e, true, out);
    }
    if (in_agg || !e.has_agg) return translate_plain(e, out);
    if (e.kind != Expr::OP) return false;
    return translate_kids(e, in_agg, out);
  }
  return translate_group(e, out);
}

/** A condition on the rows of the view (WHERE): over its columns, or its
    group columns for an aggregate view. */
bool Matcher::translate_where(const Expr &e, std::string *out) {
  if (e.has_agg) return false;
  return m_mode == Agg_mode::NONE ? translate_plain(e, out)
                                  : group_value(e, out);
}

/** Exact groups, rollup, or DISTINCT over the groups (section 3.3). */
bool Matcher::decide_mode() {
  m_grouped_query = m_q.aggregated();
  if (!m_v.is_aggregate) {
    m_mode = Agg_mode::NONE;
    return true;
  }
  if (!m_q.aggregated()) {
    if (!m_q.distinct)
      return fail("an aggregate view for a query without aggregation");
    m_mode = Agg_mode::DISTINCT;
    return true;
  }
  /* every GROUP BY expression of the query is one of the view */
  std::vector<bool> view_group_used(m_v.outputs.size(), false);
  for (const Expr_ptr &g : m_q.group) {
    bool found = false;
    for (size_t i = 0; i < m_v.outputs.size(); i++) {
      if (m_v.outputs[i].kind != View_output::GROUP) continue;
      if (match(*g, *m_v.block.select[i].expr)) {
        view_group_used[i] = true;
        found = true;
      }
    }
    if (!found) {
      /* it may still be computed from the group columns: regroup */
      std::string dummy;
      if (!group_value(*g, &dummy))
        return fail("a GROUP BY expression of the query is not computed from "
                    "the groups of the view");
      m_mode = Agg_mode::ROLLUP;
    }
  }
  if (m_mode != Agg_mode::ROLLUP) {
    bool all = true;
    for (size_t i = 0; i < m_v.outputs.size(); i++)
      if (m_v.outputs[i].kind == View_output::GROUP && !view_group_used[i])
        all = false;
    m_mode = all ? Agg_mode::EXACT : Agg_mode::ROLLUP;
  }
  return true;
}

bool Matcher::build_sql(const std::vector<std::string> &where_in,
                        Candidate *c) {
  std::vector<std::string> where = where_in;
  std::vector<std::string> having;
  const bool exact = m_mode == Agg_mode::EXACT;

  std::string sql = "SELECT ";
  if (m_q.distinct || m_mode == Agg_mode::DISTINCT) sql += "DISTINCT ";
  for (size_t i = 0; i < m_q.select.size(); i++) {
    std::string s;
    if (!translate(*m_q.select[i].expr, false, &s))
      return fail("select expression " + std::to_string(i + 1) +
                  " cannot be computed from the view");
    if (i != 0) sql += ", ";
    sql += s + " AS " + quote(m_q.select[i].name);
  }
  sql += " FROM " + quote(m_v.db) + "." + quote(m_v.name) + " " +
         quote(m_alias);
  for (size_t i = 0; i < m_q.insts.size(); i++) {
    if (m_q2v[i] >= 0) continue;
    const Instance &in = m_q.insts[i];
    sql += ", " + quote(in.db) + "." + quote(in.table) + " " + quote(in.alias);
  }
  for (size_t i = 0; i < m_q.conjuncts.size(); i++) {
    if (m_is_local[i]) continue;
    std::string s;
    if (!translate_where(*m_q.conjuncts[i], &s))
      return fail("a join condition cannot be computed from the view");
    where.push_back("(" + s + ")");
  }
  for (const Expr_ptr &h : m_q.having) {
    std::string s;
    if (!translate(*h, false, &s))
      return fail("a HAVING condition cannot be computed from the view");
    (exact ? where : having).push_back("(" + s + ")");
  }
  if (!where.empty()) {
    sql += " WHERE ";
    for (size_t i = 0; i < where.size(); i++)
      sql += (i ? " AND " : "") + where[i];
  }
  if (!exact && !m_q.group.empty()) {
    sql += " GROUP BY ";
    for (size_t i = 0; i < m_q.group.size(); i++) {
      std::string s;
      const bool ok = m_mode == Agg_mode::NONE
                          ? translate_plain(*m_q.group[i], &s)
                          : group_value(*m_q.group[i], &s);
      if (!ok) return fail("a GROUP BY expression cannot be computed");
      sql += (i ? ", " : "") + s;
    }
  }
  if (!having.empty()) {
    sql += " HAVING ";
    for (size_t i = 0; i < having.size(); i++)
      sql += (i ? " AND " : "") + having[i];
  }
  if (!m_q.order.empty()) {
    sql += " ORDER BY ";
    for (size_t i = 0; i < m_q.order.size(); i++) {
      const Order_item &o = m_q.order[i];
      std::string s;
      if (o.position > 0) {
        s = std::to_string(o.position);
      } else if (!translate(*o.expr, false, &s)) {
        return fail("an ORDER BY expression cannot be computed");
      }
      sql += (i ? ", " : "") + s + (o.desc ? " DESC" : "");
    }
  }
  sql += m_q.limit;
  c->sql = std::move(sql);
  return true;
}

bool Matcher::load_tables() {
  for (const Instance &in : m_v.block.insts) {
    const Table_info *info = m_st->catalog->get(in.db, in.table);
    if (info == nullptr || !info->base_table)
      return fail("table " + in.db + "." + in.table + " cannot be read");
    m_vinfo.push_back(info);
  }
  return true;
}

bool Matcher::classify_view() {
  for (const Expr_ptr &c : m_v.block.conjuncts) {
    Pred p;
    if (!classify(*c, false, &p))
      return fail("a column of the view is not in its table any more");
    m_view_preds.push_back(p);
  }
  for (const Pred &p : m_view_preds)
    if (p.kind == Pred::EQ) m_vc.merge(p.a, p.b);
  return true;
}

bool Matcher::analyse(std::vector<bool> *in_hub, bool *filters,
                      std::vector<std::string> *residuals) {
  m_hub_mode = true;
  if (!load_tables() || !classify_view()) return false;
  *filters = false;
  for (const Pred &p : m_view_preds) {
    if (p.kind == Pred::RESIDUAL) residuals->push_back(p.expr->sig);
    /* an equality of two columns of one table filters too */
    std::set<int> insts;
    p.expr->instances(&insts);
    if (p.kind != Pred::EQ || insts.size() < 2) *filters = true;
  }
  if (!eliminate_extra_tables()) return false;
  *in_hub = m_alive;
  return true;
}

bool Matcher::run(Candidate *candidate) {
  if (m_alias.empty()) return fail("no alias for the view");
  /* the definitions of the tables, as they are now */
  m_view_table = m_st->catalog->get(m_v.db, m_v.name);
  if (m_view_table == nullptr || !m_view_table->mview_table)
    return fail("the table of the view cannot be read");
  m_warp_view = same_ci(m_v.engine, "WARP");
  if (!load_tables()) return false;
  m_out_ok.assign(m_v.outputs.size(), -1);
  for (size_t i = 0; i < m_v.outputs.size(); i++) {
    const Expr &e = *m_v.block.select[i].expr;
    if (e.kind != Expr::COLUMN) continue;
    if (m_v.outputs[i].kind != View_output::PLAIN &&
        m_v.outputs[i].kind != View_output::GROUP)
      continue;
    m_simple_outputs[col_id(e.inst, e.column)].push_back(i);
  }

  /* the conjuncts of the query over S, and the others */
  m_is_local.assign(m_q.conjuncts.size(), false);
  for (size_t i = 0; i < m_q.conjuncts.size(); i++) {
    std::set<int> insts;
    m_q.conjuncts[i]->instances(&insts);
    bool local = !insts.empty();
    for (int inst : insts)
      if (m_q2v[inst] < 0) local = false;
    m_is_local[i] = local;
    if (!local) continue;
    Pred p;
    if (!classify(*m_q.conjuncts[i], true, &p))
      return fail("a column of the query is not in its table any more");
    m_local.push_back(p);
  }
  if (!classify_view()) return false;
  for (const Pred &p : m_local)
    if (p.kind == Pred::EQ) m_qc.merge(p.a, p.b);

  if (!eliminate_extra_tables()) return false;
  for (const auto &eq : m_fk_equalities) m_qc.merge(eq.first, eq.second);

  if (!decide_mode()) return false;

  std::vector<std::string> where;
  if (!compensate(&where)) return false;

  /* the same statement for both: EXACT may fail on an aggregate that
     only a regrouping computes */
  if (!build_sql(where, candidate)) {
    if (m_mode != Agg_mode::EXACT) return false;
    why.clear();
    m_mode = Agg_mode::ROLLUP;
    if (!build_sql(where, candidate)) return false;
  }

  int replaced = 0, extra = 0;
  for (int q : m_v2q) (q >= 0 ? replaced : extra)++;
  candidate->score = {replaced,
                      m_v.is_aggregate ? 1 : 0,
                      m_mode == Agg_mode::EXACT ? 1 : 0,
                      -extra,
                      -m_compensations};
  return true;
}

/** Can the user read the table? */
bool can_select(THD *thd, const std::string &db, const std::string &table) {
  Table_ref tr(db.c_str(), db.size(), table.c_str(), table.size(),
               table.c_str(), TL_READ);
  return !check_table_access(thd, SELECT_ACL, &tr, false, 1, true);
}

bool same_table(const Instance &a, const Instance &b) {
  return a.db == b.db && a.table == b.table;
}

/** The mappings of the view instances to the query (recursively). */
void enumerate(const View &view, const Block &q, bool partial, size_t j,
               std::vector<int> *v2q, std::vector<bool> *used,
               std::vector<std::vector<int>> *out, size_t limit) {
  if (out->size() >= limit) return;
  if (j == v2q->size()) {
    size_t mapped = 0;
    for (int x : *v2q)
      if (x >= 0) mapped++;
    if (mapped == 0) return;
    if ((!partial || view.is_aggregate) && mapped != q.insts.size()) return;
    out->push_back(*v2q);
    return;
  }
  for (size_t i = 0; i < q.insts.size(); i++) {
    if ((*used)[i] || !same_table(view.block.insts[j], q.insts[i])) continue;
    (*used)[i] = true;
    (*v2q)[j] = static_cast<int>(i);
    enumerate(view, q, partial, j + 1, v2q, used, out, limit);
    (*used)[i] = false;
  }
  if (!view.in_hub[j]) {
    (*v2q)[j] = -1;
    enumerate(view, q, partial, j + 1, v2q, used, out, limit);
  }
}

/** The keys of the statement for the filter tree. */
void query_keys(const Registry &registry, const Block &q, Query_keys *keys) {
  for (const Instance &in : q.insts) {
    const int id = registry.id(in.db + "." + in.table);
    if (id >= 0) keys->tables.push_back(id);
  }
  for (const Expr_ptr &c : q.conjuncts) {
    const int id = registry.id(c->sig);
    if (id >= 0) keys->residuals.push_back(id);
  }
  for (auto *v : {&keys->tables, &keys->residuals}) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
  }
}

/** The statement is something the plugin may rewrite at all. */
bool check_statement(THD *thd, std::string *why) {
  LEX *lex = thd->lex;
  if (lex->sql_command != SQLCOM_SELECT) {
    *why = "not a SELECT";
    return false;
  }
  if (lex->result != nullptr) {
    *why = "SELECT ... INTO";
    return false;
  }
  if (lex->is_explain() && (lex->explain_format->is_explain_into() ||
                            lex->explain_format->is_explain_for_schema())) {
    *why = "EXPLAIN ... INTO or FOR SCHEMA";
    return false;
  }
  return true;
}

/** The statement is one query block (no WITH: see rewrite_ctes()). */
bool check_single_block(THD *thd, std::string *why) {
  LEX *lex = thd->lex;
  Query_expression *unit = lex->unit;
  if (!unit->is_simple() || lex->query_block->next_query_block() != nullptr ||
      lex->query_block != unit->first_query_block()) {
    *why = "set operations";
    return false;
  }
  if (lex->query_block->active_options() & OPTION_FOUND_ROWS) {
    *why = "SQL_CALC_FOUND_ROWS";
    return false;
  }
  return true;
}

std::string explain_prefix(LEX *lex) {
  if (!lex->is_explain()) return "";
  std::string p = "EXPLAIN ";
  if (lex->is_explain_analyze) p += "ANALYZE ";
  if (lex->explain_format->is_hierarchical())
    p += "FORMAT=JSON ";
  else if (lex->explain_format->is_tree())
    p += "FORMAT=TREE ";
  else
    p += "FORMAT=TRADITIONAL ";
  return p;
}

}  // namespace

bool analyse_view_tables(THD *thd, View *view, Catalog *catalog,
                         std::vector<std::string> *residuals,
                         std::string *why) {
  static const Options options;
  Block no_query;
  Statement st(thd, options, &no_query, catalog, nullptr, nullptr);
  Matcher matcher(&st, *view,
                  std::vector<int>(view->block.insts.size(), -1));
  if (!matcher.analyse(&view->in_hub, &view->filters, residuals)) {
    *why = matcher.why;
    return true;
  }
  return false;
}

namespace {

/**
  Rewrite one query block: the statement, or the query expression of a
  common table expression.
  @param[out] sql  the block, reading a view
*/
bool rewrite_block(THD *thd, Query_block *qb, const Registry &registry,
                   const Options &options, std::string *sql,
                   std::vector<std::string> *notes, Counters *counters) {
  std::string why;

  /* nothing to do if no view has a table of the statement */
  bool known = false;
  for (Table_ref *tr = qb->get_table_list(); tr != nullptr;
       tr = tr->next_local) {
    if (tr->db != nullptr && tr->table_name != nullptr &&
        registry.id(std::string(tr->db) + "." + tr->table_name) >= 0)
      known = true;
  }
  if (!known) {
    if (options.trace)
      notes->push_back("no materialized view has a table of the query");
    return false;
  }

  Catalog catalog(thd, false);
  Block q;
  if (make_block(thd, qb, &catalog, false, &q, &why)) {
    if (options.trace) notes->push_back("the query is not used: " + why);
    return false;
  }

  Statement st(thd, options, &q, &catalog, notes, counters);
  st.only_full_group_by =
      (thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY) != 0;

  /* the candidate views */
  std::vector<int> views;
  if (options.use_filter_tree) {
    Query_keys keys;
    query_keys(registry, q, &keys);
    registry.tree.search(keys, q.aggregated() || q.distinct, &views);
  } else {
    for (size_t i = 0; i < registry.views.size(); i++)
      views.push_back(static_cast<int>(i));
  }

  Candidate best;
  bool found = false;
  for (int index : views) {
    const View &view = registry.views[index];
    if (view.refresh_mode == Mview_refresh_mode::SYNCHRONOUS) {
      if (!mview_is_maintained(view.db, view.name, view.epoch)) {
        st.note(view, "not maintained now (the WARP_MVIEW daemon does not "
                      "keep it current)");
        continue;
      }
      if (!options.sync_current) {
        st.note(view, "the WARP_MVIEW daemon has not applied the last "
                      "transaction of this session yet");
        continue;
      }
    } else if (!options.use_manual) {
      st.note(view, "REFRESH MANUAL (see mview_rewrite_use_manual_views)");
      continue;
    }
    if (view.time_zone_dependent && !options.session_utc) {
      st.note(view, "it has TIMESTAMP values, which are computed in UTC, and "
                    "the time zone of the session is not UTC");
      continue;
    }
    if (view.is_aggregate && !q.aggregated() && !q.distinct) {
      st.note(view, "an aggregate view for a query without aggregation");
      continue;
    }
    counters->candidates++;
    std::vector<std::vector<int>> mappings;
    std::vector<int> v2q(view.block.insts.size(), -1);
    std::vector<bool> used(q.insts.size(), false);
    enumerate(view, q, options.partial, 0, &v2q, &used, &mappings, 256);
    if (mappings.empty()) {
      st.note(view, "its tables are not the tables of the query");
      continue;
    }
    bool privilege_checked = false;
    for (const std::vector<int> &mapping : mappings) {
      size_t replaced = 0;
      for (int x : mapping)
        if (x >= 0) replaced++;
      /* a join view that neither joins nor filters is not cheaper than
         its one table */
      if (!view.is_aggregate && replaced == 1 && !view.filters) {
        st.note(view, "it would replace one table without filtering it");
        continue;
      }
      Matcher matcher(&st, view, mapping);
      Candidate c;
      if (!matcher.run(&c)) {
        st.note(view, matcher.why);
        continue;
      }
      if (!privilege_checked) {
        privilege_checked = true;
        if (!can_select(thd, view.db, view.name)) {
          st.note(view, "no SELECT privilege on the view");
          break;
        }
      }
      st.note(view, "matches: " + c.sql);
      if (!found || c.score > best.score) {
        best = std::move(c);
        found = true;
      }
    }
  }
  if (!found) return false;
  /* the statement must be allowed as it is: then it is rewritten */
  for (const Instance &in : q.insts) {
    if (!can_select(thd, in.db, in.table)) {
      if (options.trace)
        notes->push_back("not rewritten: no SELECT privilege on " +
                         quote(in.db) + "." + quote(in.table));
      return false;
    }
  }
  *sql = best.sql;
  return true;
}

/** Every reference to a common table expression in and under a block. */
void collect_cte_references(Query_block *qb, std::vector<Table_ref *> *out,
                            int depth) {
  if (depth > 64) return;
  for (Table_ref *tr = qb->get_table_list(); tr != nullptr;
       tr = tr->next_local) {
    if (tr->common_table_expr() != nullptr &&
        tr->derived_query_expression() != nullptr)
      out->push_back(tr);
  }
  for (Query_expression *unit = qb->first_inner_query_expression();
       unit != nullptr; unit = unit->next_query_expression()) {
    for (Query_block *inner = unit->first_query_block(); inner != nullptr;
         inner = inner->next_query_block())
      collect_cte_references(inner, out, depth + 1);
  }
}

/**
  A statement with a WITH clause: the query expression of every common
  table expression that is one query block is rewritten as if it were a
  statement of its own, and put in the text of the statement in the place
  of the original.  The rest of the statement is as it was.

  The blocks are the ones the parser made for the references to the common
  table expressions: a name in them that is another common table
  expression is a derived table there, not a base table of that name.
*/
bool rewrite_ctes(THD *thd, const Registry &registry, const Options &options,
                  std::string *sql, std::vector<std::string> *notes,
                  Counters *counters) {
  LEX *lex = thd->lex;
  PT_with_clause *with = lex->unit->m_with_clause;
  const char *text = thd->query().str;
  const size_t length = thd->query().length;

  std::vector<Table_ref *> references;
  for (Query_block *qb = lex->unit->first_query_block(); qb != nullptr;
       qb = qb->next_query_block())
    collect_cte_references(qb, &references, 0);

  struct Replacement {
    size_t offset, length;
    std::string sql;
  };
  std::vector<Replacement> replacements;
  for (PT_common_table_expr *cte : with->elements()) {
    const std::string name =
        "common table expression " +
        quote(std::string(cte->name().str, cte->name().length));
    Table_ref *reference = nullptr;
    for (Table_ref *tr : references) {
      if (cte->is(tr->common_table_expr())) {
        reference = tr;
        break;
      }
    }
    if (reference == nullptr) continue;  // not used: not parsed
    std::string why;
    Query_expression *unit = reference->derived_query_expression();
    Query_block *qb = unit->first_query_block();
    if (reference->common_table_expr()->recursive)
      why = "recursive";
    else if (!unit->is_simple() || qb == nullptr ||
             qb->next_query_block() != nullptr)
      why = "set operations";
    else if (unit->m_with_clause != nullptr)
      why = "WITH";
    else if (qb->active_options() & OPTION_FOUND_ROWS)
      why = "SQL_CALC_FOUND_ROWS";
    /* where its text is: ( query expression ) */
    const POS &pos = cte->subquery_pos();
    if (why.empty() &&
        (pos.raw.start == nullptr || pos.raw.start < text ||
         pos.raw.end > text + length || pos.raw.end < pos.raw.start + 2 ||
         *pos.raw.start != '(' || *(pos.raw.end - 1) != ')'))
      why = "its text is not in the statement";
    if (!why.empty()) {
      if (options.trace)
        notes->push_back(name + ": the query is not used: " + why);
      continue;
    }
    std::string block_sql;
    std::vector<std::string> block_notes;
    const bool rewritten = rewrite_block(thd, qb, registry, options,
                                         &block_sql, &block_notes, counters);
    for (const std::string &note : block_notes)
      notes->push_back(name + ": " + note);
    if (!rewritten) continue;
    Replacement r;
    r.offset = static_cast<size_t>(pos.raw.start - text);
    r.length = static_cast<size_t>(pos.raw.end - pos.raw.start);
    r.sql = "(" + block_sql + ")";
    replacements.push_back(std::move(r));
  }
  if (replacements.empty()) return false;

  std::sort(replacements.begin(), replacements.end(),
            [](const Replacement &a, const Replacement &b) {
              return a.offset < b.offset;
            });
  sql->clear();
  size_t at = 0;
  for (const Replacement &r : replacements) {
    if (r.offset < at) return false;  // they cannot overlap
    sql->append(text + at, r.offset - at);
    sql->append(r.sql);
    at = r.offset + r.length;
  }
  sql->append(text + at, length - at);
  return true;
}

}  // namespace

bool rewrite_statement(THD *thd, const Registry &registry,
                       const Options &options, std::string *sql,
                       std::vector<std::string> *notes, Counters *counters) {
  std::string why;
  if (!check_statement(thd, &why)) {
    if (options.trace) notes->push_back("the query is not used: " + why);
    return false;
  }
  if (thd->lex->unit->m_with_clause != nullptr)
    return rewrite_ctes(thd, registry, options, sql, notes, counters);
  if (!check_single_block(thd, &why)) {
    if (options.trace) notes->push_back("the query is not used: " + why);
    return false;
  }
  std::string block_sql;
  if (!rewrite_block(thd, thd->lex->query_block, registry, options,
                     &block_sql, notes, counters))
    return false;
  *sql = explain_prefix(thd->lex) + block_sql;
  return true;
}

}  // namespace mv_rewrite
