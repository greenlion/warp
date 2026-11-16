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

/**
  @file storage/rapid/rapid_sql.cc

  The statement that the RAPID secondary engine gives to DuckDB.

  It is written from the query as MySQL has resolved it (the tree of
  Query_expression, Query_block, Table_ref and Item), not from the text that
  MySQL prints for it: that text is MySQL's own, with constructs that only
  its executor knows.  The tree is read after the statement is prepared and
  before it is optimized (handlerton::prepare_secondary_engine): the
  optimizer changes the conditions in place.

  What the resolver did to the query is written back as SQL:

    - a subquery that was made a semijoin: the tables of the subquery are a
      nest in the join list and its condition is part of the WHERE clause
      (or of the ON clause of the outer join that it was in).  It is written
      as EXISTS (SELECT 1 FROM <tables of the nest> WHERE <the conjuncts
      that read a table of the nest>);
    - an antijoin: NOT EXISTS (SELECT 1 FROM <nest> WHERE <its condition>);
    - an IN subquery that was not made a semijoin has conditions that compare
      its columns with the outer expression (IN to EXISTS): they are left
      out, it is written as <expression> IN (<subquery>);
    - derived tables, views and common table expressions that were merged
      are tables of the query block; the others are written as subqueries
      in FROM;
    - the wrappers of ROLLUP and of cached constants are removed.

  Every table of the statement gets an alias of its own (t1, t2, ...), all
  columns are written with it: the names that the user gave need not be
  unique after derived tables are merged.

  MySQL sorts NULL first in ascending order and last in descending order,
  DuckDB the other way round: ORDER BY says NULLS FIRST / NULLS LAST.

  Anything that is not known here makes the generator fail with a reason;
  the statement is then not run by the secondary engine.
*/

#include "storage/rapid/rapid_sql.h"

#include <map>
#include <string>
#include <vector>

#include "my_time.h"
#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_strfunc.h"
#include "sql/item_subselect.h"
#include "sql/item_sum.h"
#include "sql/item_timefunc.h"
#include "sql/nested_join.h"
#include "sql/parse_tree_nodes.h"
#include "sql/query_term.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "sql/window.h"
#include "sql_string.h"

namespace rapid {

namespace {

class Generator {
 public:
  explicit Generator(THD *thd) : m_thd(thd) {}

  bool run(Query_expression *unit, std::string *sql, std::string *why) {
    std::string out = gen_unit(unit, false);
    if (m_failed) {
      *why = m_why;
      return true;
    }
    *sql = out;
    return false;
  }

 private:
  THD *m_thd;
  bool m_failed{false};
  std::string m_why;
  std::map<const Table_ref *, std::string> m_alias;
  int m_next_alias{0};

  /* A grouped query block whose select list, HAVING or ORDER BY is being
     written.  MySQL takes a column that is not in GROUP BY there when its
     value is the same for all the rows of a group (the column of a derived
     table of one row, a column that the grouped ones determine); DuckDB
     only takes the expressions of GROUP BY.  Such a column is written as
     ANY_VALUE(column), which is that value. */
  struct Group_scope {
    std::vector<std::string> expressions;  ///< the GROUP BY expressions
    std::vector<const Table_ref *> tables; ///< the tables of the block
  };
  Group_scope *m_group{nullptr};
  bool m_in_aggregate{false};
  /// the tables that gen_table has named for the block that is written
  std::vector<const Table_ref *> *m_block_tables{nullptr};

  std::string fail(const std::string &why) {
    if (!m_failed) {
      m_failed = true;
      m_why = why;
    }
    return "NULL";
  }

  static std::string quote_id(const char *name, size_t length) {
    std::string out = "\"";
    for (size_t i = 0; i < length; i++) {
      if (name[i] == '"') out += '"';
      out += name[i];
    }
    out += '"';
    return out;
  }
  static std::string quote_id(const char *name) {
    return quote_id(name, name == nullptr ? 0 : strlen(name));
  }
  static std::string quote_str(const char *text, size_t length) {
    std::string out = "'";
    for (size_t i = 0; i < length; i++) {
      if (text[i] == '\'') out += '\'';
      out += text[i];
    }
    out += '\'';
    return out;
  }

  /* ---------------------------------------------------------------- */
  /* Query expressions                                                 */
  /* ---------------------------------------------------------------- */

  std::string gen_unit(Query_expression *unit, bool skip_in2exists) {
    if (unit == nullptr) return fail("no query expression");
    /* a scope of its own */
    Group_scope *const saved_group = m_group;
    const bool saved_in_aggregate = m_in_aggregate;
    std::vector<const Table_ref *> *const saved_tables = m_block_tables;
    m_group = nullptr;
    m_in_aggregate = false;
    m_block_tables = nullptr;
    const std::string text =
        unit->is_simple() ? gen_block(unit->first_query_block(), skip_in2exists)
                          : gen_term(unit->query_term(), true);
    m_group = saved_group;
    m_in_aggregate = saved_in_aggregate;
    m_block_tables = saved_tables;
    return text;
  }

  /** ORDER BY and LIMIT of a set operation or of a parenthesis. */
  std::string gen_term_tail(Query_term *term) {
    Query_block *qb = term->query_block();
    std::string out;
    if (qb == nullptr) return out;
    if (qb->order_list.elements > 0)
      out += " ORDER BY " + gen_order_list(qb->order_list.first, true);
    out += gen_limit(qb);
    return out;
  }

  std::string gen_term(Query_term *term, bool top) {
    switch (term->term_type()) {
      case QT_QUERY_BLOCK:
        return "(" + gen_block(term->query_block(), false) + ")";
      case QT_UNARY: {
        Query_term_unary *unary = down_cast<Query_term_unary *>(term);
        std::string out = gen_term(unary->child(0), false) + gen_term_tail(term);
        return top ? out : "(" + out + ")";
      }
      case QT_UNION:
      case QT_INTERSECT:
      case QT_EXCEPT: {
        Query_term_set_op *op = down_cast<Query_term_set_op *>(term);
        const char *name = term->term_type() == QT_UNION
                               ? " UNION "
                               : term->term_type() == QT_INTERSECT
                                     ? " INTERSECT "
                                     : " EXCEPT ";
        std::string out;
        for (size_t i = 0; i < op->child_count(); i++) {
          if (i > 0) {
            out += name;
            /* the operands up to the last DISTINCT one are distinct */
            if (static_cast<int64_t>(i) > op->last_distinct()) out += "ALL ";
          }
          out += gen_term(op->child(i), false);
        }
        out += gen_term_tail(term);
        return top ? out : "(" + out + ")";
      }
      default:
        return fail("an unknown query term");
    }
  }

  std::string gen_limit(Query_block *qb) {
    std::string out;
    if (qb->select_limit != nullptr) {
      if (!qb->select_limit->const_item()) return fail("a LIMIT that is not constant");
      out += " LIMIT " + std::to_string(qb->select_limit->val_uint());
      if (qb->offset_limit != nullptr) {
        if (!qb->offset_limit->const_item())
          return fail("an OFFSET that is not constant");
        out += " OFFSET " + std::to_string(qb->offset_limit->val_uint());
      }
    }
    return out;
  }

  /* ---------------------------------------------------------------- */
  /* Query blocks                                                      */
  /* ---------------------------------------------------------------- */

  /** A condition as its conjuncts. */
  static void conjuncts(Item *cond, std::vector<Item *> *out) {
    if (cond == nullptr) return;
    if (cond->type() == Item::COND_ITEM &&
        down_cast<Item_cond *>(cond)->functype() == Item_func::COND_AND_FUNC) {
      List_iterator<Item> it(*down_cast<Item_cond *>(cond)->argument_list());
      Item *arg;
      while ((arg = it++)) conjuncts(arg, out);
      return;
    }
    out->push_back(cond);
  }

  static table_map nest_tables(const Table_ref *tr) {
    if (tr->nested_join == nullptr) return tr->map();
    table_map map = 0;
    for (const Table_ref *inner : tr->nested_join->m_tables)
      map |= nest_tables(inner);
    return map;
  }

  /**
    The tables of a join list and the condition of its level (the WHERE
    clause, or the ON clause of the nest that the list is of).

    @param tables      the list (in the reverse of the order of the query)
    @param cond        the condition of the level
    @param skip_in2exists  leave out the conditions that IN to EXISTS added
    @param[out] from   the tables, joined
    @param[out] where  the condition, with the semijoins and antijoins of
                       the list as EXISTS and NOT EXISTS; empty if none
  */
  void gen_join_list(const mem_root_deque<Table_ref *> &tables, Item *cond,
                     bool skip_in2exists, std::string *from,
                     std::string *where) {
    std::vector<Table_ref *> list(tables.begin(), tables.end());
    std::reverse(list.begin(), list.end());
    std::vector<Item *> conds;
    conjuncts(cond, &conds);
    gen_join_vector(list, conds, skip_in2exists, from, where);
  }

  /** gen_join_list with the tables in the order of the query and the
      condition as its conjuncts. */
  void gen_join_vector(const std::vector<Table_ref *> &list,
                       const std::vector<Item *> &conds, bool skip_in2exists,
                       std::string *from, std::string *where) {
    std::vector<bool> taken(conds.size(), false);
    std::vector<std::string> predicates;

    bool first = true;
    for (Table_ref *tr : list) {
      if (tr->is_sj_or_aj_nest()) {
        if (tr->is_aj_nest()) {
          std::string inner_from, inner_where;
          gen_join_list(tr->nested_join->m_tables, tr->join_cond(), false,
                        &inner_from, &inner_where);
          predicates.push_back("NOT EXISTS (SELECT 1 FROM " + inner_from +
                               (inner_where.empty() ? "" : " WHERE " + inner_where) +
                               ")");
          continue;
        }
        /* A semijoin: the conjuncts of this level that read a table of the
           nest are its condition. */
        const table_map inner_map = nest_tables(tr);
        std::vector<Item *> mine;
        for (size_t i = 0; i < conds.size(); i++) {
          if (!taken[i] && (conds[i]->used_tables() & inner_map) != 0) {
            taken[i] = true;
            mine.push_back(conds[i]);
          }
        }
        std::vector<Table_ref *> inner(tr->nested_join->m_tables.begin(),
                                       tr->nested_join->m_tables.end());
        std::reverse(inner.begin(), inner.end());
        /* The tables of the nest that no conjunct connects are subqueries
           of their own: EXISTS (a, b WHERE ca AND cb) is EXISTS (a WHERE
           ca) AND EXISTS (b WHERE cb).  (An IN subquery with EXISTS
           subqueries that do not read its tables is one nest; as one
           EXISTS it would be their cross product.) */
        std::vector<table_map> maps;
        std::vector<int> component(inner.size());
        for (size_t i = 0; i < inner.size(); i++) {
          maps.push_back(nest_tables(inner[i]));
          component[i] = static_cast<int>(i);
        }
        auto find = [&component](int x) {
          while (component[x] != x) x = component[x];
          return x;
        };
        bool outer_join_inside = false;
        for (Table_ref *t : inner)
          if (t->outer_join) outer_join_inside = true;
        for (Item *item : mine) {
          int root = -1;
          for (size_t i = 0; i < inner.size(); i++) {
            if ((item->used_tables() & maps[i]) == 0) continue;
            if (root < 0)
              root = find(static_cast<int>(i));
            else
              component[find(static_cast<int>(i))] = root;
          }
        }
        if (outer_join_inside)  // the order of the tables matters: one part
          for (size_t i = 1; i < inner.size(); i++)
            component[find(static_cast<int>(i))] = find(0);
        for (size_t c = 0; c < inner.size(); c++) {
          if (find(static_cast<int>(c)) != static_cast<int>(c)) continue;
          std::vector<Table_ref *> part;
          table_map part_map = 0;
          for (size_t i = 0; i < inner.size(); i++) {
            if (find(static_cast<int>(i)) == static_cast<int>(c)) {
              part.push_back(inner[i]);
              part_map |= maps[i];
            }
          }
          std::vector<Item *> part_conds;
          for (Item *item : mine)
            if ((item->used_tables() & part_map) != 0) part_conds.push_back(item);
          std::string inner_from, inner_where;
          gen_join_vector(part, part_conds, false, &inner_from, &inner_where);
          predicates.push_back("EXISTS (SELECT 1 FROM " + inner_from +
                               (inner_where.empty() ? "" : " WHERE " + inner_where) +
                               ")");
        }
        continue;
      }

      std::string table;
      std::string on;
      if (tr->nested_join != nullptr) {
        std::string inner_from, inner_where;
        gen_join_list(tr->nested_join->m_tables, tr->join_cond(), false,
                      &inner_from, &inner_where);
        table = "(" + inner_from + ")";
        on = inner_where;
      } else {
        table = gen_table(tr);
        if (tr->join_cond() != nullptr) on = gen_item(tr->join_cond());
      }
      if (first) {
        *from += table;
        /* a condition of the first table (a merged view) is of the level */
        if (!on.empty()) predicates.push_back(on);
      } else if (tr->outer_join) {
        *from += " LEFT JOIN " + table + " ON " + (on.empty() ? "TRUE" : on);
      } else if (!on.empty()) {
        *from += " INNER JOIN " + table + " ON " + on;
      } else {
        *from += " CROSS JOIN " + table;
      }
      first = false;
    }
    if (first) *from += "(SELECT 1) AS \"dual\"";

    std::string text;
    for (size_t i = 0; i < conds.size(); i++) {
      if (taken[i]) continue;
      if (skip_in2exists && conds[i]->created_by_in2exists()) continue;
      if (!text.empty()) text += " AND ";
      text += gen_item(conds[i]);
    }
    for (const std::string &predicate : predicates) {
      if (!text.empty()) text += " AND ";
      text += predicate;
    }
    *where = text;
  }

  /** A table of a join list: a base table, or a derived table. */
  std::string gen_table(Table_ref *tr) {
    const std::string alias = "t" + std::to_string(++m_next_alias);
    m_alias[tr] = alias;
    if (m_block_tables != nullptr) m_block_tables->push_back(tr);
    if (tr->is_table_function()) return fail("a table function");
    if (tr->is_derived()) {
      Query_expression *unit = tr->derived_query_expression();
      if (unit == nullptr) return fail("a derived table without a query");
      if (unit->m_lateral_deps != 0) return fail("a lateral derived table");
      std::string out = "(" + gen_unit(unit, false) + ") AS " + alias;
      /* the names that the columns are read by */
      if (tr->table != nullptr && tr->table->s->fields > 0) {
        out += "(";
        for (uint i = 0; i < tr->table->s->fields; i++) {
          if (i > 0) out += ", ";
          out += quote_id(tr->table->field[i]->field_name);
        }
        out += ")";
      }
      return out;
    }
    if (tr->is_view()) return fail("a view that was not merged");
    if (tr->db == nullptr || tr->table_name == nullptr)
      return fail("a table without a name");
    return quote_id(tr->db) + "." + quote_id(tr->table_name) + " AS " + alias;
  }

  std::string gen_block(Query_block *qb, bool skip_in2exists) {
    if (qb == nullptr) return fail("no query block");
    if (qb->is_table_value_constructor) return fail("a table value constructor");
    if (qb->is_recursive()) return fail("a recursive common table expression");

    /* the tables first: the columns are written with their aliases */
    std::string from, where;
    std::vector<const Table_ref *> block_tables;
    std::vector<const Table_ref *> *const saved_tables = m_block_tables;
    Group_scope *const saved_group = m_group;
    const bool saved_in_aggregate = m_in_aggregate;
    m_block_tables = &block_tables;
    m_group = nullptr;
    m_in_aggregate = false;
    gen_join_list(qb->m_table_nest, qb->where_cond(), skip_in2exists, &from,
                  &where);
    m_block_tables = nullptr;

    /* GROUP BY before the select list: the select list is written with it */
    Group_scope scope;
    scope.tables = block_tables;
    std::string groups;
    for (ORDER *group = qb->group_list.first; group != nullptr;
         group = group->next) {
      const std::string text = gen_item(*group->item);
      if (!groups.empty()) groups += ", ";
      groups += text;
      scope.expressions.push_back(text);
    }
    const bool grouped = qb->is_grouped();
    if (grouped) m_group = &scope;

    std::string out = "SELECT ";
    if (qb->is_distinct()) out += "DISTINCT ";
    bool first = true;
    for (Item *item : qb->visible_fields()) {
      if (!first) out += ", ";
      first = false;
      out += gen_item(item);
      if (item->item_name.is_set())
        out += " AS " + quote_id(item->item_name.ptr(), item->item_name.length());
    }
    if (first) return fail("a query block without columns");
    out += " FROM " + from;
    if (!where.empty()) out += " WHERE " + where;

    if (qb->group_list.elements > 0) {
      switch (qb->olap) {
        case UNSPECIFIED_OLAP_TYPE:
          out += " GROUP BY " + groups;
          break;
        case ROLLUP_TYPE:
          out += " GROUP BY ROLLUP (" + groups + ")";
          break;
        case CUBE_TYPE:
          out += " GROUP BY CUBE (" + groups + ")";
          break;
        default:
          return fail("GROUPING SETS");
      }
    }
    if (qb->having_cond() != nullptr) {
      std::vector<Item *> conds;
      conjuncts(qb->having_cond(), &conds);
      std::string having;
      for (Item *cond : conds) {
        if (skip_in2exists && cond->created_by_in2exists()) continue;
        if (!having.empty()) having += " AND ";
        having += gen_item(cond);
      }
      if (!having.empty()) out += " HAVING " + having;
    }
    if (qb->qualify_cond() != nullptr)
      out += " QUALIFY " + gen_item(qb->qualify_cond());
    if (qb->order_list.elements > 0)
      out += " ORDER BY " + gen_order_list(qb->order_list.first, false);
    out += gen_limit(qb);
    m_group = saved_group;
    m_in_aggregate = saved_in_aggregate;
    m_block_tables = saved_tables;
    return out;
  }

  /**
    @param by_name  the list of a set operation: its expressions are the
                    columns of the result, which are named
  */
  std::string gen_order_list(ORDER *order, bool by_name) {
    std::string out;
    for (; order != nullptr; order = order->next) {
      if (!out.empty()) out += ", ";
      Item *item = *order->item;
      if (by_name && item->real_item()->type() == Item::FIELD_ITEM) {
        Item_field *field = down_cast<Item_field *>(item->real_item());
        out += quote_id(field->field_name);
      } else {
        out += gen_item(item);
      }
      /* as MySQL sorts NULL: the smallest value */
      out += order->direction == ORDER_DESC ? " DESC NULLS LAST"
                                            : " ASC NULLS FIRST";
    }
    return out;
  }

  /* ---------------------------------------------------------------- */
  /* Expressions                                                       */
  /* ---------------------------------------------------------------- */

  std::string gen_args(Item_func *func, const char *separator, uint from = 0) {
    std::string out;
    for (uint i = from; i < func->argument_count(); i++) {
      if (i > from) out += separator;
      out += gen_item(func->arguments()[i]);
    }
    return out;
  }

  std::string gen_value(Item *item) {
    if (item->is_null()) return "NULL";
    StringBuffer<128> buffer;
    String *value = item->val_str(&buffer);
    if (value == nullptr) return "NULL";
    switch (item->result_type()) {
      case INT_RESULT:
      case DECIMAL_RESULT:
      case REAL_RESULT:
        if (!item->is_temporal())
          return std::string(value->ptr(), value->length());
        break;
      default:
        break;
    }
    const std::string text = quote_str(value->ptr(), value->length());
    switch (item->data_type()) {
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
        return "DATE " + text;
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP:
        return "TIMESTAMP " + text;
      case MYSQL_TYPE_TIME:
        return "TIME " + text;
      default:
        return text;
    }
  }

  std::string decimal_type(const Item *item) {
    int precision = item->decimal_precision();
    int scale = item->decimals;
    if (scale > 30) scale = 30;
    if (precision > 38) precision = 38;
    if (precision < scale + 1) precision = scale + 1;
    if (precision > 38) {
      precision = 38;
      if (scale > 37) scale = 37;
    }
    return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
  }

  /** The type of DuckDB for a value of the type of an item (for CAST). */
  std::string type_of(const Item *item) {
    switch (item->data_type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_YEAR:
        return item->unsigned_flag ? "UBIGINT" : "BIGINT";
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
        return decimal_type(item);
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
        return "DOUBLE";
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
        return "DATE";
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP:
        return "TIMESTAMP";
      case MYSQL_TYPE_TIME:
        return "TIME";
      default:
        return "VARCHAR";
    }
  }

  static const char *interval_unit(interval_type type) {
    switch (type) {
      case INTERVAL_YEAR:
        return "YEAR";
      case INTERVAL_QUARTER:
        return "QUARTER";
      case INTERVAL_MONTH:
        return "MONTH";
      case INTERVAL_WEEK:
        return "WEEK";
      case INTERVAL_DAY:
        return "DAY";
      case INTERVAL_HOUR:
        return "HOUR";
      case INTERVAL_MINUTE:
        return "MINUTE";
      case INTERVAL_SECOND:
        return "SECOND";
      case INTERVAL_MICROSECOND:
        return "MICROSECOND";
      default:
        return nullptr;
    }
  }

  std::string gen_field(Item_field *item) {
    if (item->field == nullptr && item->m_table_ref == nullptr)
      return fail("a column that is not resolved");
    const char *name =
        item->field != nullptr ? item->field->field_name : item->field_name;
    auto alias = m_alias.find(item->m_table_ref);
    if (alias == m_alias.end()) {
      if (item->m_table_ref != nullptr)
        return fail(std::string("a column of a table that is not in the "
                                "statement: ") +
                    (name != nullptr ? name : "?"));
      return quote_id(name);
    }
    return alias->second + "." + quote_id(name);
  }

  std::string gen_case(Item_func_case *item) {
    const uint count = item->argument_count();
    const int first = item->get_first_expr_num();
    uint pairs = first >= 0 ? static_cast<uint>(first)
                            : (count % 2 == 0 ? count : count - 1);
    const bool has_else = count > pairs + (first >= 0 ? 1 : 0);
    Item **args = item->arguments();
    std::string out = "CASE";
    if (first >= 0) out += " " + gen_item(args[first]);
    for (uint i = 0; i + 1 < pairs; i += 2)
      out += " WHEN " + gen_item(args[i]) + " THEN " + gen_item(args[i + 1]);
    if (has_else) out += " ELSE " + gen_item(args[count - 1]);
    return out + " END";
  }

  std::string gen_cast(Item_func *func) {
    const std::string arg = gen_item(func->arguments()[0]);
    const std::string name = func->func_name();
    if (name == "cast_as_signed") return "CAST(" + arg + " AS BIGINT)";
    if (name == "cast_as_unsigned") return "CAST(" + arg + " AS UBIGINT)";
    if (name == "cast_as_decimal")
      return "CAST(" + arg + " AS " + decimal_type(func) + ")";
    if (name == "cast_as_real" || name == "cast_as_double" ||
        name == "cast_as_float")
      return "CAST(" + arg + " AS DOUBLE)";
    if (name == "cast_as_date") return "CAST(" + arg + " AS DATE)";
    if (name == "cast_as_datetime") return "CAST(" + arg + " AS TIMESTAMP)";
    if (name == "cast_as_time") return "CAST(" + arg + " AS TIME)";
    if (name == "cast_as_char") return "CAST(" + arg + " AS VARCHAR)";
    if (name == "cast_as_year") return "CAST(" + arg + " AS BIGINT)";
    return fail("the conversion " + name);
  }

  std::string gen_subquery(Item_subselect *item) {
    Query_expression *unit = item->query_expr();
    switch (item->subquery_type()) {
      case Item_subselect::SCALAR_SUBQUERY:
        return "(" + gen_unit(unit, false) + ")";
      case Item_subselect::EXISTS_SUBQUERY: {
        Item_exists_subselect *exists = down_cast<Item_exists_subselect *>(item);
        const std::string text = "EXISTS (" + gen_unit(unit, false) + ")";
        switch (exists->value_transform) {
          case Item::BOOL_IDENTITY:
          case Item::BOOL_IS_TRUE:
            return text;
          case Item::BOOL_NEGATED:
          case Item::BOOL_IS_FALSE:
            return "(NOT " + text + ")";
          default:
            return fail("a truth test of EXISTS");
        }
      }
      case Item_subselect::IN_SUBQUERY: {
        Item_in_subselect *in = down_cast<Item_in_subselect *>(item);
        if (in->left_expr == nullptr) return fail("IN without an expression");
        std::string left;
        if (in->left_expr->type() == Item::ROW_ITEM) {
          left = "(";
          for (uint i = 0; i < in->left_expr->cols(); i++) {
            if (i > 0) left += ", ";
            left += gen_item(in->left_expr->element_index(i));
          }
          left += ")";
        } else {
          left = gen_item(in->left_expr);
        }
        /* without the comparisons that IN to EXISTS put into the subquery */
        const std::string text = left + " IN (" + gen_unit(unit, true) + ")";
        switch (in->value_transform) {
          case Item::BOOL_IDENTITY:
          case Item::BOOL_IS_TRUE:
            return "(" + text + ")";
          case Item::BOOL_NEGATED:
            return "(NOT (" + text + "))";
          case Item::BOOL_IS_FALSE:
            return "((" + text + ") IS FALSE)";
          default:
            return fail("a truth test of IN");
        }
      }
      default:
        return fail("a quantified comparison (ALL / ANY)");
    }
  }

  std::string gen_window(const Window *window) {
    std::string out = " OVER (";
    bool space = false;
    const PT_order_list *partition = window->effective_partition_by();
    if (partition != nullptr && partition->value.elements > 0) {
      out += "PARTITION BY ";
      bool first = true;
      for (ORDER *order = partition->value.first; order != nullptr;
           order = order->next) {
        if (!first) out += ", ";
        first = false;
        out += gen_item(*order->item);
      }
      space = true;
    }
    const PT_order_list *order_by = window->effective_order_by();
    if (order_by != nullptr && order_by->value.elements > 0) {
      if (space) out += " ";
      out += "ORDER BY " + gen_order_list(order_by->value.first, false);
      space = true;
    }
    const PT_frame *frame = window->frame();
    if (frame != nullptr && !frame->m_originally_absent) {
      if (space) out += " ";
      switch (frame->m_query_expression) {
        case WFU_ROWS:
          out += "ROWS";
          break;
        case WFU_RANGE:
          out += "RANGE";
          break;
        default:
          return fail("a GROUPS frame");
      }
      out += " BETWEEN " + gen_border(frame->m_from) + " AND " +
             gen_border(frame->m_to);
    }
    return out + ")";
  }

  std::string gen_border(const PT_border *border) {
    switch (border->m_border_type) {
      case WBT_CURRENT_ROW:
        return "CURRENT ROW";
      case WBT_UNBOUNDED_PRECEDING:
        return "UNBOUNDED PRECEDING";
      case WBT_UNBOUNDED_FOLLOWING:
        return "UNBOUNDED FOLLOWING";
      case WBT_VALUE_PRECEDING:
      case WBT_VALUE_FOLLOWING: {
        if (border->m_date_time) return fail("an interval frame border");
        return gen_item(border->border()) +
               (border->m_border_type == WBT_VALUE_PRECEDING ? " PRECEDING"
                                                             : " FOLLOWING");
      }
    }
    return fail("an unknown frame border");
  }

  std::string gen_sum(Item_sum *sum) {
    /* the wrapper of ROLLUP: the aggregate that it switches */
    if (Item_rollup_sum_switcher *switcher =
            dynamic_cast<Item_rollup_sum_switcher *>(sum))
      return gen_item(switcher->master());
    /* The arguments of an aggregate are of the rows, not of the groups (a
       window function is computed from the groups, its arguments are not
       taken out of the rules of the groups). */
    const bool was_in_aggregate = m_in_aggregate;
    if (!sum->m_is_window_function) m_in_aggregate = true;
    const std::string text = gen_sum_inner(sum);
    m_in_aggregate = was_in_aggregate;
    return text;
  }

  std::string gen_sum_inner(Item_sum *sum) {
    std::string out;
    const std::string distinct = sum->has_with_distinct() ? "DISTINCT " : "";
    bool cast_decimal = false;
    switch (sum->sum_func()) {
      case Item_sum::ROLLUP_SUM_SWITCHER_FUNC:
        return gen_item(down_cast<Item_rollup_sum_switcher *>(sum)->master());
      case Item_sum::COUNT_FUNC:
      case Item_sum::COUNT_DISTINCT_FUNC:
        if (!sum->has_with_distinct() && sum->argument_count() == 1 &&
            sum->get_arg(0)->const_item() && !sum->get_arg(0)->is_null())
          out = "COUNT(*)";
        else
          out = "COUNT(" + distinct + gen_args(sum, ", ") + ")";
        break;
      case Item_sum::SUM_FUNC:
      case Item_sum::SUM_DISTINCT_FUNC:
        out = "SUM(" + distinct + gen_args(sum, ", ") + ")";
        break;
      case Item_sum::AVG_FUNC:
      case Item_sum::AVG_DISTINCT_FUNC:
        out = "AVG(" + distinct + gen_args(sum, ", ") + ")";
        cast_decimal = sum->result_type() == DECIMAL_RESULT;
        break;
      case Item_sum::MIN_FUNC:
        out = "MIN(" + gen_args(sum, ", ") + ")";
        break;
      case Item_sum::MAX_FUNC:
        out = "MAX(" + gen_args(sum, ", ") + ")";
        break;
      case Item_sum::STD_FUNC:
      case Item_sum::VARIANCE_FUNC: {
        const std::string name = sum->func_name();
        const char *duck = name == "std" || name == "stddev" ||
                                   name == "stddev_pop"
                               ? "STDDEV_POP"
                               : name == "stddev_samp"
                                     ? "STDDEV_SAMP"
                                     : name == "var_samp"
                                           ? "VAR_SAMP"
                                           : name == "variance" ||
                                                     name == "var_pop"
                                                 ? "VAR_POP"
                                                 : nullptr;
        if (duck == nullptr) return fail("the aggregate " + name);
        out = std::string(duck) + "(" + gen_args(sum, ", ") + ")";
        break;
      }
      case Item_sum::ROW_NUMBER_FUNC:
        out = "ROW_NUMBER()";
        break;
      case Item_sum::RANK_FUNC:
        out = "RANK()";
        break;
      case Item_sum::DENSE_RANK_FUNC:
        out = "DENSE_RANK()";
        break;
      case Item_sum::CUME_DIST_FUNC:
        out = "CUME_DIST()";
        break;
      case Item_sum::PERCENT_RANK_FUNC:
        out = "PERCENT_RANK()";
        break;
      case Item_sum::NTILE_FUNC:
        out = "NTILE(" + gen_args(sum, ", ") + ")";
        break;
      default:
        return fail(std::string("the aggregate or window function ") +
                    sum->func_name());
    }
    if (sum->m_is_window_function) {
      if (sum->window() == nullptr) return fail("a window function without a window");
      out += gen_window(sum->window());
    }
    /* AVG of an exact value is a DECIMAL of MySQL, a DOUBLE of DuckDB */
    if (cast_decimal) out = "CAST(" + out + " AS " + decimal_type(sum) + ")";
    return out;
  }

  /**
    An operand of AND, OR and NOT.  MySQL takes any value for a truth value:
    a number that is not zero is true, a string is the number that it
    starts with ('abc' is 0, false).
  */
  std::string gen_truth(Item *item) {
    const std::string text = gen_item(item);
    if (item->is_bool_func() || item->type() == Item::SUBQUERY_ITEM ||
        item->type() == Item::COND_ITEM)
      return text;
    if (item->type() == Item::FUNC_ITEM) {
      switch (down_cast<Item_func *>(item)->functype()) {
        case Item_func::TRIG_COND_FUNC:
        case Item_func::TRUE_FUNC:
        case Item_func::FALSE_FUNC:
          return text;
        default:
          break;
      }
      if (std::string(down_cast<Item_func *>(item)->func_name()) ==
          "<in_optimizer>")
        return text;
    }
    switch (item->result_type()) {
      case INT_RESULT:
      case DECIMAL_RESULT:
      case REAL_RESULT:
        if (!item->is_temporal()) return "(" + text + " <> 0)";
        return "(" + text + " IS NOT NULL)";
      default:
        return "(CASE WHEN " + text + " IS NULL THEN NULL ELSE COALESCE(TRY_CAST(" +
               text + " AS DOUBLE), 0) <> 0 END)";
    }
  }

  std::string gen_func(Item_func *func) {
    Item **args = func->arguments();
    const uint count = func->argument_count();
    auto binary = [&](const char *op) {
      return "(" + gen_item(args[0]) + " " + op + " " + gen_item(args[1]) + ")";
    };
    /* A comparison of a number with a string: MySQL compares them as
       numbers (a string that is not a number is 0), DuckDB fails. */
    auto is_number = [](Item *item) {
      return !item->is_temporal() &&
             (item->result_type() == INT_RESULT ||
              item->result_type() == DECIMAL_RESULT ||
              item->result_type() == REAL_RESULT);
    };
    auto is_string = [](Item *item) {
      return !item->is_temporal() && item->result_type() == STRING_RESULT &&
             item->type() != Item::NULL_ITEM;
    };
    auto compare = [&](const char *op) {
      std::string left = gen_item(args[0]);
      std::string right = gen_item(args[1]);
      auto as_number = [](const std::string &text) {
        return "(CASE WHEN " + text + " IS NULL THEN NULL ELSE COALESCE(TRY_CAST(" +
               text + " AS DOUBLE), 0) END)";
      };
      if (is_number(args[0]) && is_string(args[1])) right = as_number(right);
      if (is_string(args[0]) && is_number(args[1])) left = as_number(left);
      return "(" + left + " " + op + " " + right + ")";
    };
    switch (func->functype()) {
      case Item_func::COND_AND_FUNC:
      case Item_func::COND_OR_FUNC: {
        Item_cond *cond = down_cast<Item_cond *>(func);
        List_iterator<Item> it(*cond->argument_list());
        std::string out;
        Item *arg;
        while ((arg = it++)) {
          if (!out.empty())
            out += func->functype() == Item_func::COND_AND_FUNC ? " AND " : " OR ";
          out += gen_truth(arg);
        }
        return out.empty() ? "TRUE" : "(" + out + ")";
      }
      case Item_func::XOR_FUNC:
        return "((" + gen_item(args[0]) + ") <> (" + gen_item(args[1]) + "))";
      case Item_func::NOT_FUNC:
        return "(NOT " + gen_truth(args[0]) + ")";
      case Item_func::EQ_FUNC:
        return compare("=");
      case Item_func::EQUAL_FUNC:
        return binary("IS NOT DISTINCT FROM");
      case Item_func::NE_FUNC:
        return compare("<>");
      case Item_func::LT_FUNC:
        return compare("<");
      case Item_func::LE_FUNC:
        return compare("<=");
      case Item_func::GE_FUNC:
        return compare(">=");
      case Item_func::GT_FUNC:
        return compare(">");
      case Item_func::LIKE_FUNC: {
        if (count > 2) return fail("LIKE with ESCAPE");
        return binary("LIKE");
      }
      case Item_func::ISNULL_FUNC:
        return "(" + gen_item(args[0]) + " IS NULL)";
      case Item_func::ISNOTNULL_FUNC:
        return "(" + gen_item(args[0]) + " IS NOT NULL)";
      case Item_func::ISTRUTH_FUNC: {
        const std::string name = func->func_name();
        const char *test = name == "istrue"
                               ? "IS TRUE"
                               : name == "isnottrue"
                                     ? "IS NOT TRUE"
                                     : name == "isfalse"
                                           ? "IS FALSE"
                                           : name == "isnotfalse"
                                                 ? "IS NOT FALSE"
                                                 : nullptr;
        if (test == nullptr) return fail("the truth test " + name);
        return "((" + gen_item(args[0]) + ") " + test + ")";
      }
      case Item_func::BETWEEN: {
        Item_func_opt_neg *between = down_cast<Item_func_opt_neg *>(func);
        return "(" + gen_item(args[0]) +
               (between->negated ? " NOT BETWEEN " : " BETWEEN ") +
               gen_item(args[1]) + " AND " + gen_item(args[2]) + ")";
      }
      case Item_func::IN_FUNC: {
        Item_func_opt_neg *in = down_cast<Item_func_opt_neg *>(func);
        return "(" + gen_item(args[0]) + (in->negated ? " NOT IN (" : " IN (") +
               gen_args(func, ", ", 1) + "))";
      }
      case Item_func::PLUS_FUNC:
        return binary("+");
      case Item_func::MINUS_FUNC:
        return binary("-");
      case Item_func::MUL_FUNC:
        return binary("*");
      case Item_func::DIV_FUNC: {
        /* an exact division is a DECIMAL of MySQL with the scale of the
           dividend plus div_precision_increment, a DOUBLE of DuckDB */
        const std::string text = binary("/");
        if (func->result_type() == DECIMAL_RESULT)
          return "CAST(" + text + " AS " + decimal_type(func) + ")";
        return text;
      }
      case Item_func::MOD_FUNC:
        return binary("%");
      case Item_func::NEG_FUNC:
        return "(-" + gen_item(args[0]) + ")";
      case Item_func::CASE_FUNC:
        return gen_case(down_cast<Item_func_case *>(func));
      case Item_func::IF_FUNC:
      case Item_func::BOOL_IF_FUNC:
        return "CASE WHEN " + gen_item(args[0]) + " THEN " + gen_item(args[1]) +
               " ELSE " + gen_item(args[2]) + " END";
      case Item_func::NULLIF_FUNC:
        return "NULLIF(" + gen_item(args[0]) + ", " + gen_item(args[1]) + ")";
      case Item_func::COALESCE_FUNC:
        return "COALESCE(" + gen_args(func, ", ") + ")";
      case Item_func::TYPECAST_FUNC:
        return gen_cast(func);
      case Item_func::COLLATE_FUNC:
        return gen_item(args[0]);
      case Item_func::ROLLUP_GROUP_ITEM_FUNC:
        return gen_item(args[0]);
      case Item_func::GROUPING_FUNC:
        return "GROUPING(" + gen_args(func, ", ") + ")";
      case Item_func::TRIG_COND_FUNC:
        return gen_item(args[0]);
      case Item_func::TRUE_FUNC:
        return "TRUE";
      case Item_func::FALSE_FUNC:
        return "FALSE";
      case Item_func::DATETIME_LITERAL:
        return gen_value(func);
      case Item_func::DATEADD_FUNC: {
        Item_func_add_interval *add = down_cast<Item_func_add_interval *>(func);
        const char *unit = interval_unit(add->get_interval_type());
        if (unit == nullptr) return fail("a compound interval");
        return "(" + gen_item(args[0]) + (add->is_subtract() ? " - " : " + ") +
               "INTERVAL (" + gen_item(args[1]) + ") " + unit + ")";
      }
      case Item_func::YEAR_FUNC:
        return "YEAR(" + gen_item(args[0]) + ")";
      case Item_func::MONTH_FUNC:
        return "MONTH(" + gen_item(args[0]) + ")";
      case Item_func::DAY_FUNC:
        return "DAY(" + gen_item(args[0]) + ")";
      case Item_func::QUARTER_FUNC:
        return "QUARTER(" + gen_item(args[0]) + ")";
      case Item_func::HOUR_FUNC:
        return "HOUR(" + gen_item(args[0]) + ")";
      case Item_func::MINUTE_FUNC:
        return "MINUTE(" + gen_item(args[0]) + ")";
      case Item_func::SECOND_FUNC:
        return "SECOND(" + gen_item(args[0]) + ")";
      case Item_func::DATE_FUNC:
        return "CAST(" + gen_item(args[0]) + " AS DATE)";
      case Item_func::ABS_FUNC:
        return "ABS(" + gen_item(args[0]) + ")";
      case Item_func::ROUND_FUNC:
        return "ROUND(" + gen_args(func, ", ") + ")";
      case Item_func::FLOOR_FUNC:
        return "FLOOR(" + gen_item(args[0]) + ")";
      case Item_func::CEILING_FUNC:
        return "CEIL(" + gen_item(args[0]) + ")";
      case Item_func::SQRT_FUNC:
        return "SQRT(" + gen_item(args[0]) + ")";
      case Item_func::GREATEST_FUNC:
        return "GREATEST(" + gen_args(func, ", ") + ")";
      case Item_func::LEAST_FUNC:
        return "LEAST(" + gen_args(func, ", ") + ")";
      default:
        break;
    }

    /* by name */
    const std::string name = func->func_name();
    /* the wrapper of an IN subquery that was not made a semijoin: the
       subquery writes its left expression itself */
    if (name == "<in_optimizer>" && count >= 1) return gen_item(args[count - 1]);
    /* CONCAT of MySQL is NULL when an argument is NULL, as || is; concat()
       of DuckDB leaves the NULL out */
    if (name == "concat" && count >= 1) {
      std::string out = "(";
      for (uint i = 0; i < count; i++) {
        if (i > 0) out += " || ";
        out += "CAST(" + gen_item(args[i]) + " AS VARCHAR)";
      }
      return out + ")";
    }
    /* The check of a scalar subquery that was made a derived table: more
       than one row for a row of the query is an error. */
    if (name == "reject_if" && count == 1)
      return "(CASE WHEN " + gen_truth(args[0]) +
             " THEN error('Subquery returns more than 1 row') ELSE TRUE END)";
    struct Named {
      const char *mysql;
      const char *duck;
      int min_args;
      int max_args;
    };
    static const Named named[] = {
        {"ifnull", "COALESCE", 2, 2},
        {"upper", "UPPER", 1, 1},
        {"lower", "LOWER", 1, 1},
        {"substr", "SUBSTR", 2, 3},
        {"substring", "SUBSTR", 2, 3},
        {"left", "LEFT", 2, 2},
        {"right", "RIGHT", 2, 2},
        {"trim", "TRIM", 1, 1},
        {"ltrim", "LTRIM", 1, 1},
        {"rtrim", "RTRIM", 1, 1},
        {"char_length", "LENGTH", 1, 1},
        {"length", "STRLEN", 1, 1},
        {"replace", "REPLACE", 3, 3},
        {"truncate", "TRUNC", 1, 2},
        {"pow", "POW", 2, 2},
        {"power", "POW", 2, 2},
        {"dayofmonth", "DAY", 1, 1},
        {"dayofweek", "DAYOFWEEK", 1, 1},
        {"dayofyear", "DAYOFYEAR", 1, 1},
        {"week", "WEEK", 1, 1},
    };
    for (const Named &entry : named) {
      if (name == entry.mysql) {
        if (static_cast<int>(count) < entry.min_args ||
            static_cast<int>(count) > entry.max_args)
          return fail("the function " + name + " with " +
                      std::to_string(count) + " arguments");
        return std::string(entry.duck) + "(" + gen_args(func, ", ") + ")";
      }
    }
    return fail("the function " + name);
  }

  std::string gen_item(Item *item) {
    if (item == nullptr) return fail("an empty expression");
    if (m_failed) return "NULL";
    if (m_group != nullptr && !m_in_aggregate) {
      /* an expression of GROUP BY is written as GROUP BY has it */
      Group_scope *const scope = m_group;
      m_group = nullptr;
      const std::string plain = gen_item_inner(item);
      m_group = scope;
      for (const std::string &expression : scope->expressions)
        if (expression == plain) return plain;
      if (item->real_item()->type() == Item::FIELD_ITEM) {
        const Table_ref *table =
            down_cast<Item_field *>(item->real_item())->m_table_ref;
        for (const Table_ref *mine : scope->tables)
          if (mine == table) return "ANY_VALUE(" + plain + ")";
        return plain;  // a column of an outer query block
      }
    }
    return gen_item_inner(item);
  }

  std::string gen_item_inner(Item *item) {
    switch (item->type()) {
      case Item::FIELD_ITEM:
        return gen_field(down_cast<Item_field *>(item));
      case Item::REF_ITEM: {
        Item_ref *ref = down_cast<Item_ref *>(item);
        if (ref->ref_pointer() == nullptr || ref->ref_item() == nullptr)
          return fail("a reference that is not resolved");
        return gen_item(ref->ref_item());
      }
      case Item::CACHE_ITEM: {
        Item_cache *cache = down_cast<Item_cache *>(item);
        if (cache->get_example() != nullptr) return gen_item(cache->get_example());
        return gen_value(item);
      }
      case Item::NULL_ITEM:
        return "NULL";
      case Item::INT_ITEM:
      case Item::DECIMAL_ITEM:
      case Item::REAL_ITEM:
      case Item::STRING_ITEM:
        return gen_value(item);
      case Item::FUNC_ITEM:
      case Item::COND_ITEM:
        return gen_func(down_cast<Item_func *>(item));
      case Item::SUM_FUNC_ITEM:
        return gen_sum(down_cast<Item_sum *>(item));
      case Item::SUBQUERY_ITEM:
        return gen_subquery(down_cast<Item_subselect *>(item));
      case Item::ROW_ITEM: {
        std::string out = "(";
        for (uint i = 0; i < item->cols(); i++) {
          if (i > 0) out += ", ";
          out += gen_item(item->element_index(i));
        }
        return out + ")";
      }
      default:
        return fail("an expression of a kind that is not known (" +
                    std::to_string(static_cast<int>(item->type())) + ")");
    }
  }
};

}  // namespace

bool generate_duckdb_sql(THD *thd, LEX *lex, std::string *sql,
                         std::string *why) {
  if (lex == nullptr || lex->unit == nullptr) {
    *why = "no statement";
    return true;
  }
  Generator generator(thd);
  return generator.run(lex->unit, sql, why);
}

}  // namespace rapid
