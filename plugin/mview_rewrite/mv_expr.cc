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
  @file plugin/mview_rewrite/mv_expr.cc

  Expressions and query blocks made from a parsed (not resolved) query
  block, and the comparison of literals for ranges.

  An operator is printed with Item::print() after its operands that are
  not constant have been replaced by placeholder columns; the text around
  the placeholders becomes Expr::pieces.  Printing an expression with other
  operands (columns of a view) then only puts their SQL between the pieces.
  Constant operands stay in the text.
*/

#include "plugin/mview_rewrite/mview_rewrite.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "decimal.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"
#include "mysql_time.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_row.h"
#include "sql/item_sum.h"
#include "sql/item_timefunc.h"
#include "sql-common/my_decimal.h"
#include "sql/nested_join.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/table.h"
#include "sql_string.h"

namespace mv_rewrite {

std::string quote(const std::string &name) {
  std::string res;
  res.reserve(name.size() + 2);
  res.push_back('`');
  for (const char c : name) {
    if (c == '`') res.push_back('`');
    res.push_back(c);
  }
  res.push_back('`');
  return res;
}

void Expr::instances(std::set<int> *out) const {
  if (kind == COLUMN) out->insert(inst);
  for (const Expr_ptr &kid : kids) kid->instances(out);
}

/* ------------------------------------------------------------------ */
/* Literals                                                            */
/* ------------------------------------------------------------------ */

namespace {

bool is_ascii(const std::string &s) {
  for (const unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}

/** A date or datetime literal as a packed datetime. */
bool temporal_value(const Literal &lit, bool date_only, long long *packed) {
  if (lit.kind != Literal::STRING && lit.kind != Literal::TEMPORAL)
    return false;
  if (!is_ascii(lit.bytes)) return false;
  MYSQL_TIME ltime;
  MYSQL_TIME_STATUS status;
  if (str_to_datetime(lit.bytes.data(), lit.bytes.size(), &ltime, 0, &status))
    return false;
  if (status.warnings != 0) return false;
  if (ltime.time_type == MYSQL_TIMESTAMP_DATE) {
    /* nothing */
  } else if (ltime.time_type == MYSQL_TIMESTAMP_DATETIME) {
    if (date_only) return false;
  } else {
    return false;
  }
  *packed = TIME_to_longlong_datetime_packed(ltime);
  return true;
}

int compare_doubles(double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); }

}  // namespace

bool compare_literals(const Literal &a, const Literal &b,
                      const Column_info &column, int *result) {
  switch (column.tclass) {
    case Type_class::INT:
    case Type_class::DECIMAL:
    case Type_class::REAL: {
      if (a.kind != Literal::NUMBER || b.kind != Literal::NUMBER) return false;
      if (a.exact && b.exact && column.tclass != Type_class::REAL) {
        my_decimal da, db;
        const char *end_a = a.number.data() + a.number.size();
        const char *end_b = b.number.data() + b.number.size();
        if (str2my_decimal(E_DEC_ERROR, a.number.data(), &da, &end_a) != 0 ||
            str2my_decimal(E_DEC_ERROR, b.number.data(), &db, &end_b) != 0)
          return false;
        *result = my_decimal_cmp(&da, &db);
        return true;
      }
      *result = compare_doubles(std::strtod(a.number.c_str(), nullptr),
                                std::strtod(b.number.c_str(), nullptr));
      return true;
    }
    case Type_class::STRING: {
      if (a.kind != Literal::STRING || b.kind != Literal::STRING) return false;
      const CHARSET_INFO *cs = column.charset;
      if (cs == nullptr || cs->mbminlen != 1) return false;
      if (!is_ascii(a.bytes) || !is_ascii(b.bytes)) return false;
      const int r = cs->coll->strnncollsp(
          cs, pointer_cast<const uchar *>(a.bytes.data()), a.bytes.size(),
          pointer_cast<const uchar *>(b.bytes.data()), b.bytes.size());
      *result = r < 0 ? -1 : (r > 0 ? 1 : 0);
      return true;
    }
    case Type_class::DATE:
    case Type_class::DATETIME:
    case Type_class::TIMESTAMP: {
      const bool date_only = column.tclass == Type_class::DATE;
      long long pa, pb;
      if (!temporal_value(a, date_only, &pa) ||
          !temporal_value(b, date_only, &pb))
        return false;
      *result = pa < pb ? -1 : (pa > pb ? 1 : 0);
      return true;
    }
    default:
      return false;
  }
}

bool literal_fits(const Literal &lit, const Column_info &column) {
  int dummy;
  return compare_literals(lit, lit, column, &dummy);
}

bool equivalence_compatible(const Column_info &a, const Column_info &b) {
  if (a.tclass != b.tclass || a.tclass == Type_class::NONE) return false;
  if (a.tclass == Type_class::STRING) return a.charset == b.charset;
  return true;
}

/* ------------------------------------------------------------------ */
/* Building expressions                                                */
/* ------------------------------------------------------------------ */

namespace {

/** Functions whose evaluation has effects or depends on how often they
    are evaluated: a statement with them is not rewritten. */
bool banned_function(const char *name) {
  static const char *const banned[] = {
      "sleep",          "get_lock",          "release_lock",
      "release_all_locks", "is_free_lock",   "is_used_lock",
      "benchmark",      "last_insert_id",    "found_rows",
      "row_count",      "master_pos_wait",   "source_pos_wait",
      "wait_for_executed_gtid_set", "wait_until_sql_thread_after_gtids",
      "nextval",        "setval",            "lastval"};
  for (const char *b : banned)
    if (my_strcasecmp(system_charset_info, name, b) == 0) return true;
  return false;
}

/** What a reference in a clause may resolve to. */
enum class Clause {
  PLAIN,     ///< FROM columns (WHERE, ON, select list)
  GROUP_BY,  ///< columns, then select expressions
  HAVING,    ///< columns, then select expressions
  ORDER_BY   ///< select expressions, then columns
};

class Builder {
 public:
  Builder(THD *thd, Block *block, bool view, std::string *why)
      : m_thd(thd), m_block(block), m_view(view), m_why(why) {}

  Expr_ptr build(Item *item, Clause clause);
  /** The expression of select item `index` (a copy). */
  Expr_ptr select_expr(size_t index);

  bool fail(const std::string &why) {
    if (m_why->empty()) *m_why = why;
    return true;
  }

 private:
  Expr_ptr fail_null(const std::string &why) {
    fail(why);
    return nullptr;
  }
  Expr_ptr column(Item_ident *ident, Clause clause);
  bool resolve_column(const char *db, const char *table, const char *field,
                      int *inst, std::string *column, bool *ambiguous);
  int resolve_alias(const char *field);
  Expr_ptr literal(Item *item);
  Expr_ptr operation(Item *item, Clause clause);
  bool print_with_placeholders(Item *item, const std::vector<Item **> &refs,
                               const std::vector<bool> &replace, Expr *e);
  std::string print(const Item *item);
  Item_field *placeholder(size_t i);

  THD *m_thd;
  Block *m_block;
  bool m_view;
  std::string *m_why;
  std::vector<Item_field *> m_placeholders;
  std::vector<std::string> m_tokens;
};

std::string Builder::print(const Item *item) {
  StringBuffer<1024> buffer(m_thd->charset());
  item->print(m_thd, &buffer, QT_ORDINARY);
  return std::string(buffer.ptr(), buffer.length());
}

Item_field *Builder::placeholder(size_t i) {
  while (m_placeholders.size() <= i) {
    const std::string name = "mv$ph$" + std::to_string(m_placeholders.size());
    const char *text = m_thd->strmake(name.data(), name.size());
    Item_field *ph = text == nullptr
                         ? nullptr
                         : new (m_thd->mem_root)
                               Item_field(POS(), nullptr, nullptr, text);
    if (ph == nullptr) return nullptr;
    m_placeholders.push_back(ph);
    m_tokens.push_back(print(ph));
  }
  return m_placeholders[i];
}

Expr_ptr copy_expr(const Expr &e) {
  auto c = std::make_unique<Expr>();
  c->kind = e.kind;
  c->inst = e.inst;
  c->column = e.column;
  c->star_table = e.star_table;
  c->lit = e.lit;
  c->pieces = e.pieces;
  c->slots = e.slots;
  for (const Expr_ptr &kid : e.kids) c->kids.push_back(copy_expr(*kid));
  c->functype = e.functype;
  c->negated = e.negated;
  c->sumfunc = e.sumfunc;
  c->distinct = e.distinct;
  c->count_star = e.count_star;
  c->sig = e.sig;
  c->has_agg = e.has_agg;
  return c;
}

Expr_ptr Builder::select_expr(size_t index) {
  return copy_expr(*m_block->select[index].expr);
}

bool same_name(const char *a, const std::string &b) {
  return my_strcasecmp(system_charset_info, a, b.c_str()) == 0;
}

bool same_alias(const char *a, const std::string &b) {
  return my_strcasecmp(table_alias_charset, a, b.c_str()) == 0;
}

bool Builder::resolve_column(const char *db, const char *table,
                             const char *field, int *inst,
                             std::string *column, bool *ambiguous) {
  *inst = -1;
  *ambiguous = false;
  for (size_t i = 0; i < m_block->insts.size(); i++) {
    const Instance &in = m_block->insts[i];
    if (table != nullptr && table[0] != '\0') {
      if (!same_alias(table, in.alias)) continue;
      if (db != nullptr && db[0] != '\0') {
        /* db.table.column names a table that has no alias. */
        if (my_strcasecmp(table_alias_charset, db, in.db.c_str()) != 0 ||
            in.alias != in.table)
          continue;
      }
    }
    std::string name = field;
    if (in.info != nullptr) {
      const int c = in.info->find(field);
      if (c < 0) continue;
      name = in.info->columns[c].name;
    } else if (table == nullptr || table[0] == '\0') {
      continue;
    }
    if (*inst >= 0) {
      *ambiguous = true;
      return false;
    }
    *inst = static_cast<int>(i);
    *column = name;
  }
  return *inst >= 0;
}

int Builder::resolve_alias(const char *field) {
  int found = -1;
  for (size_t i = 0; i < m_block->select.size(); i++) {
    if (same_name(field, m_block->select[i].name)) {
      if (found >= 0) return -2;  // ambiguous
      found = static_cast<int>(i);
    }
  }
  return found;
}

bool same_column(const Expr &e, int inst, const std::string &column) {
  return e.kind == Expr::COLUMN && e.inst == inst &&
         my_strcasecmp(system_charset_info, e.column.c_str(),
                       column.c_str()) == 0;
}

Expr_ptr Builder::column(Item_ident *ident, Clause clause) {
  const char *field = ident->field_name;
  if (field == nullptr) return fail_null("a column without a name");
  if (m_view && (ident->table_name == nullptr || ident->table_name[0] == 0))
    return fail_null("an unqualified column in the definition");

  int inst;
  std::string name;
  bool ambiguous;
  const bool found = resolve_column(ident->db_name, ident->table_name, field,
                                    &inst, &name, &ambiguous);
  if (ambiguous) return fail_null(std::string("column '") + field +
                                  "' is ambiguous");
  const bool unqualified =
      ident->table_name == nullptr || ident->table_name[0] == '\0';
  int alias = -1;
  if (unqualified && clause != Clause::PLAIN) {
    alias = resolve_alias(field);
    if (alias == -2)
      return fail_null(std::string("name '") + field + "' is ambiguous");
  }
  if (found && alias >= 0) {
    /* Both: fine if the select expression is that very column. */
    if (!same_column(*m_block->select[alias].expr, inst, name))
      return fail_null(std::string("name '") + field +
                       "' is a column and a select expression");
    alias = -1;
  }
  if (alias >= 0) {
    if (clause == Clause::ORDER_BY || !found) return select_expr(alias);
  }
  if (!found)
    return fail_null(std::string("unknown column '") + field + "'");
  auto e = std::make_unique<Expr>();
  e->kind = Expr::COLUMN;
  e->inst = inst;
  e->column = name;
  e->sig = "?";
  return e;
}

Expr_ptr Builder::literal(Item *item) {
  auto e = std::make_unique<Expr>();
  e->kind = Expr::LITERAL;
  e->lit.text = print(item);
  e->sig = e->lit.text;
  switch (item->type()) {
    case Item::INT_ITEM: {
      const longlong v = item->val_int();
      e->lit.kind = Literal::NUMBER;
      e->lit.exact = true;
      e->lit.number = item->unsigned_flag
                          ? std::to_string(static_cast<ulonglong>(v))
                          : std::to_string(v);
      break;
    }
    case Item::DECIMAL_ITEM: {
      my_decimal buffer;
      const my_decimal *d = item->val_decimal(&buffer);
      if (d == nullptr) break;
      char text[DECIMAL_MAX_STR_LENGTH + 1];
      int length = sizeof(text);
      if (decimal2string(d, text, &length) != 0) break;
      e->lit.kind = Literal::NUMBER;
      e->lit.exact = true;
      e->lit.number.assign(text, length);
      break;
    }
    case Item::REAL_ITEM: {
      char text[64];
      snprintf(text, sizeof(text), "%.17g", item->val_real());
      e->lit.kind = Literal::NUMBER;
      e->lit.exact = false;
      e->lit.number = text;
      break;
    }
    case Item::STRING_ITEM: {
      String buffer;
      const String *s = item->val_str(&buffer);
      if (s == nullptr) break;
      e->lit.kind = Literal::STRING;
      e->lit.bytes.assign(s->ptr(), s->length());
      e->lit.charset = item->collation.collation;
      break;
    }
    case Item::NULL_ITEM:
      e->lit.kind = Literal::NULL_VALUE;
      break;
    case Item::FUNC_ITEM: {
      Item_func *func = down_cast<Item_func *>(item);
      if (func->functype() == Item_func::NEG_FUNC &&
          func->argument_count() == 1) {
        Item *arg = func->arguments()[0];
        if (arg->type() == Item::INT_ITEM ||
            arg->type() == Item::DECIMAL_ITEM ||
            arg->type() == Item::REAL_ITEM) {
          Expr_ptr inner = literal(arg);
          if (inner->lit.kind == Literal::NUMBER) {
            e->lit.kind = Literal::NUMBER;
            e->lit.exact = inner->lit.exact;
            const std::string &n = inner->lit.number;
            e->lit.number = !n.empty() && n[0] == '-' ? n.substr(1) : "-" + n;
          }
        }
      } else if (dynamic_cast<Item_date_literal *>(func) != nullptr ||
                 dynamic_cast<Item_datetime_literal *>(func) != nullptr) {
        /* DATE'...' and TIMESTAMP'...' (not TIME'...') */
        const std::string &t = e->lit.text;
        const size_t open = t.find('\'');
        const size_t close = t.rfind('\'');
        if (open != std::string::npos && close > open) {
          e->lit.kind = Literal::TEMPORAL;
          e->lit.bytes = t.substr(open + 1, close - open - 1);
        }
      }
      break;
    }
    default:
      break;
  }
  return e;
}

/** The operands of an item that prints as a text around them. */
bool operand_refs(Item *item, std::vector<Item **> *refs) {
  switch (item->type()) {
    case Item::FUNC_ITEM:
    case Item::SUM_FUNC_ITEM: {
      Item_func *func = down_cast<Item_func *>(item);
      for (uint i = 0; i < func->argument_count(); i++)
        refs->push_back(&func->arguments()[i]);
      return true;
    }
    case Item::COND_ITEM: {
      List_iterator<Item> it(*down_cast<Item_cond *>(item)->argument_list());
      while (it++ != nullptr) refs->push_back(it.ref());
      return true;
    }
    case Item::ROW_ITEM: {
      Item_row *row = down_cast<Item_row *>(item);
      for (uint i = 0; i < row->cols(); i++) refs->push_back(row->addr(i));
      return true;
    }
    default:
      return false;
  }
}

bool Builder::print_with_placeholders(Item *item,
                                      const std::vector<Item **> &refs,
                                      const std::vector<bool> &replace,
                                      Expr *e) {
  std::vector<Item *> saved(refs.size(), nullptr);
  for (size_t i = 0; i < refs.size(); i++) {
    if (!replace[i]) continue;
    Item_field *ph = placeholder(i);
    if (ph == nullptr) return true;
    saved[i] = *refs[i];
    *refs[i] = ph;
  }
  const std::string text = print(item);
  for (size_t i = 0; i < refs.size(); i++)
    if (replace[i]) *refs[i] = saved[i];

  /* Cut the text at the placeholders. */
  size_t pos = 0;
  std::string piece;
  while (pos < text.size()) {
    size_t best = std::string::npos;
    size_t best_len = 0;
    int best_slot = -1;
    for (size_t i = 0; i < refs.size(); i++) {
      if (!replace[i]) continue;
      const size_t at = text.find(m_tokens[i], pos);
      if (at == std::string::npos) continue;
      /* the longest token at the earliest position: `mv$ph$1` and
         `mv$ph$10` cannot both match there anyway, they are quoted */
      if (at < best || (at == best && m_tokens[i].size() > best_len)) {
        best = at;
        best_len = m_tokens[i].size();
        best_slot = static_cast<int>(i);
      }
    }
    if (best_slot < 0) {
      piece += text.substr(pos);
      break;
    }
    piece += text.substr(pos, best - pos);
    e->pieces.push_back(piece);
    piece.clear();
    e->slots.push_back(best_slot);
    pos = best + best_len;
  }
  e->pieces.push_back(piece);
  return false;
}

Expr_ptr Builder::operation(Item *item, Clause clause) {
  auto e = std::make_unique<Expr>();
  e->kind = item->type() == Item::SUM_FUNC_ITEM ? Expr::AGG : Expr::OP;

  if (item->type() == Item::FUNC_ITEM || item->type() == Item::COND_ITEM ||
      item->type() == Item::SUM_FUNC_ITEM) {
    Item_func *func = down_cast<Item_func *>(item);
    if (item->type() != Item::SUM_FUNC_ITEM) {
      e->functype = func->functype();
      switch (func->functype()) {
        case Item_func::FUNC_SP:
          return fail_null("stored functions");
        case Item_func::UDF_FUNC:
          return fail_null("loadable functions");
        case Item_func::SUSERVAR_FUNC:
          return fail_null("assignments to variables");
        case Item_func::FT_FUNC:
          return fail_null("full-text search");
        case Item_func::BETWEEN:
        case Item_func::IN_FUNC:
          e->negated = down_cast<Item_func_opt_neg *>(func)->negated;
          break;
        default:
          break;
      }
      if (banned_function(func->func_name()))
        return fail_null(std::string("function ") + func->func_name());
    }
  }
  if (item->type() == Item::SUM_FUNC_ITEM) {
    Item_sum *sum = down_cast<Item_sum *>(item);
    if (sum->m_is_window_function || sum->window() != nullptr)
      return fail_null("window functions");
    switch (sum->sum_func()) {
      case Item_sum::COUNT_FUNC:
      case Item_sum::COUNT_DISTINCT_FUNC:
      case Item_sum::SUM_FUNC:
      case Item_sum::SUM_DISTINCT_FUNC:
      case Item_sum::AVG_FUNC:
      case Item_sum::AVG_DISTINCT_FUNC:
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
      case Item_sum::STD_FUNC:
      case Item_sum::VARIANCE_FUNC:
      case Item_sum::SUM_BIT_FUNC:
        break;
      default:
        return fail_null(std::string("aggregate function ") +
                         sum->func_name());
    }
    if (clause == Clause::GROUP_BY)
      return fail_null("an aggregate function in GROUP BY");
    e->sumfunc = sum->sum_func();
    e->distinct = sum->has_with_distinct();
    e->has_agg = true;
  }

  std::vector<Item **> refs;
  if (!operand_refs(item, &refs))
    return fail_null("an expression of an unknown kind");
  std::vector<bool> replace(refs.size(), false);
  bool constant = e->kind != Expr::AGG;
  for (size_t i = 0; i < refs.size(); i++) {
    Expr_ptr kid = build(*refs[i], clause);
    if (kid == nullptr) return nullptr;
    if (kid->kind == Expr::STAR) return fail_null("* in an expression");
    if (kid->has_agg && e->kind == Expr::AGG)
      return fail_null("nested aggregate functions");
    replace[i] = kid->kind != Expr::LITERAL;
    if (replace[i]) constant = false;
    e->has_agg = e->has_agg || kid->has_agg;
    e->kids.push_back(std::move(kid));
  }
  if (e->kind == Expr::AGG && e->sumfunc == Item_sum::COUNT_FUNC &&
      e->kids.size() == 1 && e->kids[0]->kind == Expr::LITERAL &&
      e->kids[0]->lit.kind != Literal::NULL_VALUE &&
      e->kids[0]->lit.kind != Literal::OTHER)
    e->count_star = true;

  if (constant) {
    /* A constant expression: a literal for the matching. */
    return literal(item);
  }
  if (print_with_placeholders(item, refs, replace, e.get())) return nullptr;

  /* The signature: pieces with the signatures of the operands. */
  std::string sig = e->pieces[0];
  for (size_t i = 0; i < e->slots.size(); i++) {
    sig += e->kids[e->slots[i]]->sig;
    sig += e->pieces[i + 1];
  }
  e->sig = std::move(sig);
  return e;
}

Expr_ptr Builder::build(Item *item, Clause clause) {
  switch (item->type()) {
    case Item::FIELD_ITEM:
    case Item::REF_ITEM: {
      Item_ident *ident = down_cast<Item_ident *>(item);
      if (item->type() == Item::FIELD_ITEM &&
          down_cast<Item_field *>(item)->is_asterisk()) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::STAR;
        if (ident->table_name != nullptr) e->star_table = ident->table_name;
        if (ident->db_name != nullptr && ident->db_name[0] != '\0')
          return fail_null("db.table.*");
        return e;
      }
      return column(ident, clause);
    }
    case Item::INT_ITEM:
    case Item::DECIMAL_ITEM:
    case Item::REAL_ITEM:
    case Item::STRING_ITEM:
    case Item::NULL_ITEM:
    case Item::HEX_BIN_ITEM:
      return literal(item);
    case Item::FUNC_ITEM: {
      const auto functype = down_cast<Item_func *>(item)->functype();
      if (functype == Item_func::DATETIME_LITERAL) return literal(item);
      return operation(item, clause);
    }
    case Item::COND_ITEM:
    case Item::SUM_FUNC_ITEM:
    case Item::ROW_ITEM:
      return operation(item, clause);
    case Item::SUBQUERY_ITEM:
      return fail_null("subqueries");
    case Item::PARAM_ITEM:
      return fail_null("parameters");
    default:
      return fail_null("an expression of an unknown kind");
  }
}

/** Add the conjuncts of a condition. */
bool add_conjuncts(Builder *builder, Item *cond, Clause clause,
                   std::vector<Expr_ptr> *out) {
  if (cond == nullptr) return false;
  if (cond->type() == Item::COND_ITEM &&
      down_cast<Item_func *>(cond)->functype() == Item_func::COND_AND_FUNC) {
    List_iterator<Item> it(*down_cast<Item_cond *>(cond)->argument_list());
    while (Item *arg = it++)
      if (add_conjuncts(builder, arg, clause, out)) return true;
    return false;
  }
  Expr_ptr e = builder->build(cond, clause);
  if (e == nullptr) return true;
  if (e->kind == Expr::STAR) return builder->fail("* in a condition");
  out->push_back(std::move(e));
  return false;
}

/** The ON conditions of a join nest; only inner joins. */
bool add_join_conditions(Builder *builder,
                         const mem_root_deque<Table_ref *> &nest,
                         std::vector<Expr_ptr> *out) {
  for (Table_ref *tr : nest) {
    if (tr->outer_join) return builder->fail("outer joins");
    if (tr->is_natural_join || tr->natural_join != nullptr ||
        tr->join_using_fields != nullptr)
      return builder->fail("NATURAL JOIN and JOIN ... USING");
    if (tr->nested_join != nullptr &&
        add_join_conditions(builder, tr->nested_join->m_tables, out))
      return true;
    if (tr->join_cond() != nullptr &&
        add_conjuncts(builder, tr->join_cond(), Clause::PLAIN, out))
      return true;
  }
  return false;
}

}  // namespace

bool make_block(THD *thd, Query_block *qb, Catalog *catalog, bool view,
                Block *block, std::string *why) {
  Builder builder(thd, block, view, why);

  if (qb->get_table_list() == nullptr) return builder.fail("no tables");
  if (qb->first_inner_query_expression() != nullptr)
    return builder.fail("subqueries or derived tables");
  if (qb->has_windows()) return builder.fail("window functions");
  if (qb->qualify_cond() != nullptr) return builder.fail("QUALIFY");
  if (qb->olap != UNSPECIFIED_OLAP_TYPE)
    return builder.fail("WITH ROLLUP");

  for (Table_ref *tr = qb->get_table_list(); tr != nullptr;
       tr = tr->next_local) {
    if (tr->is_table_function()) return builder.fail("table functions");
    if (tr->is_derived() || tr->derived_query_expression() != nullptr)
      return builder.fail("derived tables");
    if (tr->partition_names != nullptr && tr->partition_names->elements != 0)
      return builder.fail("partition selection");
    if (tr->has_tablesample()) return builder.fail("TABLESAMPLE");
    const thr_lock_type lock = tr->lock_descriptor().type;
    if (lock != TL_READ_DEFAULT && lock != TL_READ &&
        lock != TL_READ_HIGH_PRIORITY && lock != TL_UNLOCK)
      return builder.fail("locking reads");
    if (tr->db == nullptr || tr->table_name == nullptr)
      return builder.fail("a table without a schema");
    Instance in;
    in.db = tr->db;
    in.table = tr->table_name;
    in.alias = tr->alias != nullptr ? tr->alias : tr->table_name;
    if (!view && find_temporary_table(thd, tr->db, tr->table_name) != nullptr)
      return builder.fail("temporary tables");
    if (catalog != nullptr) {
      in.info = catalog->get(in.db, in.table);
      if (in.info == nullptr)
        return builder.fail("table " + in.db + "." + in.table +
                            " cannot be read from the data dictionary now");
      if (!in.info->base_table || in.info->mview_table)
        return builder.fail("table " + in.db + "." + in.table +
                            " is not a base table");
    }
    block->insts.push_back(std::move(in));
  }

  /* FROM and WHERE */
  if (add_join_conditions(&builder, qb->m_table_nest, &block->conjuncts))
    return true;
  if (add_conjuncts(&builder, qb->where_cond(), Clause::PLAIN,
                    &block->conjuncts))
    return true;

  /* the select list */
  for (Item *item : qb->visible_fields()) {
    Expr_ptr e = builder.build(item, Clause::PLAIN);
    if (e == nullptr) return true;
    if (e->kind == Expr::STAR) {
      bool any = false;
      for (size_t i = 0; i < block->insts.size(); i++) {
        const Instance &in = block->insts[i];
        if (!e->star_table.empty() && !same_alias(e->star_table.c_str(),
                                                  in.alias))
          continue;
        if (in.info == nullptr) return builder.fail("* without definitions");
        for (const Column_info &c : in.info->columns) {
          if (c.invisible) continue;
          Select_item s;
          s.expr = std::make_unique<Expr>();
          s.expr->kind = Expr::COLUMN;
          s.expr->inst = static_cast<int>(i);
          s.expr->column = c.name;
          s.expr->sig = "?";
          s.name = c.name;
          block->select.push_back(std::move(s));
          any = true;
        }
      }
      if (!any) return builder.fail("* of an unknown table");
      continue;
    }
    Select_item s;
    if (item->item_name.is_set())
      s.name.assign(item->item_name.ptr(), item->item_name.length());
    else if (e->kind == Expr::COLUMN)
      s.name = e->column;
    /* the name must be an alias of the rewritten statement */
    if (s.name.empty() ||
        system_charset_info->cset->numchars(
            system_charset_info, s.name.data(),
            s.name.data() + s.name.size()) > NAME_CHAR_LEN * 4)
      return builder.fail("a select expression without a usable name");
    block->has_agg = block->has_agg || e->has_agg;
    s.expr = std::move(e);
    block->select.push_back(std::move(s));
  }
  block->distinct = qb->is_distinct();

  /* GROUP BY */
  for (ORDER *ord = qb->group_list.first; ord != nullptr; ord = ord->next) {
    Item *item = *ord->item;
    Expr_ptr e;
    if (item->type() == Item::INT_ITEM && item->basic_const_item()) {
      const longlong pos = item->val_int();
      if (pos < 1 || pos > static_cast<longlong>(block->select.size()))
        return builder.fail("a wrong GROUP BY position");
      e = builder.select_expr(pos - 1);
    } else {
      e = builder.build(item, Clause::GROUP_BY);
    }
    if (e == nullptr) return true;
    if (e->has_agg) return builder.fail("an aggregate function in GROUP BY");
    block->group.push_back(std::move(e));
  }

  if (view) return false;

  /* HAVING */
  if (add_conjuncts(&builder, qb->having_cond(), Clause::HAVING,
                    &block->having))
    return true;
  for (const Expr_ptr &h : block->having)
    block->has_agg = block->has_agg || h->has_agg;

  /* ORDER BY */
  for (ORDER *ord = qb->order_list.first; ord != nullptr; ord = ord->next) {
    Item *item = *ord->item;
    Order_item o;
    o.desc = ord->direction == ORDER_DESC;
    if (item->type() == Item::INT_ITEM && item->basic_const_item()) {
      const longlong pos = item->val_int();
      if (pos < 1 || pos > static_cast<longlong>(block->select.size()))
        return builder.fail("a wrong ORDER BY position");
      o.position = static_cast<int>(pos);
    } else {
      o.expr = builder.build(item, Clause::ORDER_BY);
      if (o.expr == nullptr) return true;
      if (o.expr->kind == Expr::STAR) return builder.fail("ORDER BY *");
      block->has_agg = block->has_agg || o.expr->has_agg;
    }
    block->order.push_back(std::move(o));
  }

  /* LIMIT */
  if (qb->select_limit != nullptr) {
    if (qb->select_limit->type() != Item::INT_ITEM ||
        (qb->offset_limit != nullptr &&
         qb->offset_limit->type() != Item::INT_ITEM))
      return builder.fail("LIMIT that is not a number");
    block->limit = " LIMIT ";
    if (qb->offset_limit != nullptr)
      block->limit +=
          std::to_string(static_cast<ulonglong>(qb->offset_limit->val_int())) +
          ", ";
    block->limit +=
        std::to_string(static_cast<ulonglong>(qb->select_limit->val_int()));
  }
  return false;
}

}  // namespace mv_rewrite
