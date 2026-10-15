/* Copyright (C) 2026 Justin Swanhart

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.  */

/* The aggregation of the rows of a join that the engine makes
   (warp_par_join.h), for any expressions of the columns of the tables of the join.

   The rows of the result of the join are not made: the keys of the groups and
   the arguments of the aggregates are evaluated for every row while the probe
   table is probed, by the threads that probe it, and added to the groups.  The
   groups are returned to MySQL as the rows of a scan (the tables of the join
   below the aggregation are not read by MySQL any more: the values of the
   columns that MySQL reads above the aggregation are written to the records of
   their tables), the aggregates are constants that are the values of the
   groups (see warp_par_agg.h).

   An expression is a tree of nodes: columns of the tables, constants, + - *,
   unary -, comparisons, AND, OR, NOT, CASE, IF, LIKE, IN, BETWEEN and
   EXTRACT(YEAR ...).  The values are integers and decimals (scaled integers of
   at most 18 digits, the arithmetic is made with 128 bit integers), floating
   point numbers and strings (compared in their collation).  What the engine can
   not evaluate exactly as MySQL does is left to MySQL. */

#ifndef WARP_PAR_GJA_H
#define WARP_PAR_GJA_H

/* ---------------------------------------------------------------------- */
/* values and expressions                                                 */
/* ---------------------------------------------------------------------- */

struct warp_gj_val {
  int kind = WARP_GJ_INT;
  bool null = true;
  int scale = 0;            // DEC
  __int128 i = 0;           // INT, DEC, and the result of a comparison (0 or 1)
  double d = 0;             // REAL
  const char *s = nullptr;  // STR
  size_t n = 0;
};

enum { GJN_COL, GJN_CONST, GJN_ADD, GJN_SUB, GJN_MUL, GJN_NEG, GJN_CMP, GJN_AND, GJN_OR, GJN_NOT, GJN_CASE,
       GJN_LIKE, GJN_YEAR };
enum { GJC_EQ, GJC_NE, GJC_LT, GJC_LE, GJC_GT, GJC_GE };

struct warp_gj_node {
  int op = GJN_CONST;
  int kind = WARP_GJ_INT;         // the kind of the value that it gives
  int scale = 0;
  int level = 0;                  // GJN_COL: the table (0: the probe table) and the column among its columns
  int ci = 0;
  bool temporal = false;          // GJN_COL: an integer that is a date or a time
  warp_gj_val c;                  // GJN_CONST
  std::string cstr;               // GJN_CONST of a string
  int cmp = GJC_EQ;               // GJN_CMP
  int ck = WARP_GJ_DEC;           // GJN_CMP: how the operands are compared (DEC: integers and decimals, REAL, STR)
  std::vector<int> kids;
  const CHARSET_INFO *cs = nullptr;
  bool has_else = false;          // GJN_CASE: the last kid is the ELSE
};

typedef std::vector<warp_gj_node> warp_gj_nodes;

struct warp_gj_ctx {
  const warp_pj_plan *plan;
  const std::vector<warp_gj_arr> *probe;
  size_t index;
  const uint32_t *chosen;
};

static inline void warp_gj_column_value(const warp_gj_ctx &c, const warp_gj_node &n, warp_gj_val *v) {
  v->kind = n.kind;
  v->scale = n.scale;
  const warp_gj_arr *arr;
  size_t pos;
  if(n.level == 0) {
    arr = &(*c.probe)[n.ci];
    pos = c.index;
  } else {
    const uint32_t row = c.chosen[n.level];
    if(row == WARP_PJ_NULL_ROW) {
      v->null = true;
      return;
    }
    arr = &c.plan->tables[n.level].gj[n.ci];
    pos = row;
  }
  v->null = !arr->nulls.empty() && arr->nulls[pos] != 0;
  if(v->null) {
    return;
  }
  switch(n.kind) {
    case WARP_GJ_INT:
    case WARP_GJ_DEC:
      v->i = arr->i[pos];
      break;
    case WARP_GJ_REAL:
      v->d = arr->d[pos];
      break;
    default:
      v->s = arr->s[pos].data();
      v->n = arr->s[pos].size();
  }
}

static inline double warp_gj_to_real(const warp_gj_val &v) {
  if(v.kind == WARP_GJ_REAL) return v.d;
  double d = (double)v.i;
  for(int k = 0; k < v.scale; ++k) d /= 10.0;
  return d;
}

static inline __int128 warp_gj_rescale(const warp_gj_val &v, int scale) {
  __int128 i = v.i;
  for(int k = v.scale; k < scale; ++k) i *= 10;
  return i;
}

static inline bool warp_gj_true(const warp_gj_val &v) {
  if(v.null) return false;
  return v.kind == WARP_GJ_REAL ? v.d != 0 : v.i != 0;
}

static void warp_gj_eval(const warp_gj_ctx &c, const warp_gj_nodes &nodes, int id, warp_gj_val *out);

/* the value in the kind and the scale that the node gives */
static inline void warp_gj_convert(warp_gj_val *v, int kind, int scale) {
  if(v->null) {
    v->kind = kind;
    return;
  }
  if(kind == WARP_GJ_REAL) {
    v->d = warp_gj_to_real(*v);
  } else if(kind == WARP_GJ_DEC || kind == WARP_GJ_INT) {
    v->i = warp_gj_rescale(*v, scale);
    v->scale = scale;
  }
  v->kind = kind;
}

static void warp_gj_eval(const warp_gj_ctx &c, const warp_gj_nodes &nodes, int id, warp_gj_val *out) {
  const warp_gj_node &n = nodes[id];
  switch(n.op) {
    case GJN_COL:
      warp_gj_column_value(c, n, out);
      return;
    case GJN_CONST:
      *out = n.c;
      if(n.kind == WARP_GJ_STR) {
        out->s = n.cstr.data();
        out->n = n.cstr.size();
      }
      return;
    case GJN_ADD:
    case GJN_SUB:
    case GJN_MUL: {
      warp_gj_val a, b;
      warp_gj_eval(c, nodes, n.kids[0], &a);
      warp_gj_eval(c, nodes, n.kids[1], &b);
      out->kind = n.kind;
      out->scale = n.scale;
      out->null = a.null || b.null;
      if(out->null) return;
      if(n.kind == WARP_GJ_REAL) {
        const double x = warp_gj_to_real(a), y = warp_gj_to_real(b);
        out->d = n.op == GJN_ADD ? x + y : n.op == GJN_SUB ? x - y : x * y;
      } else if(n.op == GJN_MUL) {
        out->i = a.i * b.i;
      } else {
        const __int128 x = warp_gj_rescale(a, n.scale), y = warp_gj_rescale(b, n.scale);
        out->i = n.op == GJN_ADD ? x + y : x - y;
      }
      return;
    }
    case GJN_NEG: {
      warp_gj_val a;
      warp_gj_eval(c, nodes, n.kids[0], &a);
      *out = a;
      if(!a.null) {
        out->i = -a.i;
        out->d = -a.d;
      }
      return;
    }
    case GJN_CMP: {
      warp_gj_val a, b;
      warp_gj_eval(c, nodes, n.kids[0], &a);
      warp_gj_eval(c, nodes, n.kids[1], &b);
      out->kind = WARP_GJ_INT;
      out->scale = 0;
      out->null = a.null || b.null;
      if(out->null) return;
      int r;
      if(n.ck == WARP_GJ_STR) {
        r = n.cs->coll->strnncollsp(n.cs, (const uchar *)a.s, a.n, (const uchar *)b.s, b.n);
      } else if(n.ck == WARP_GJ_REAL) {
        const double x = warp_gj_to_real(a), y = warp_gj_to_real(b);
        r = x < y ? -1 : x > y ? 1 : 0;
      } else {
        const int s = std::max(a.scale, b.scale);
        const __int128 x = warp_gj_rescale(a, s), y = warp_gj_rescale(b, s);
        r = x < y ? -1 : x > y ? 1 : 0;
      }
      bool t = false;
      switch(n.cmp) {
        case GJC_EQ: t = r == 0; break;
        case GJC_NE: t = r != 0; break;
        case GJC_LT: t = r < 0; break;
        case GJC_LE: t = r <= 0; break;
        case GJC_GT: t = r > 0; break;
        default: t = r >= 0;
      }
      out->i = t ? 1 : 0;
      return;
    }
    case GJN_AND:
    case GJN_OR: {
      const bool is_and = n.op == GJN_AND;
      bool any_null = false;
      out->kind = WARP_GJ_INT;
      out->scale = 0;
      for(int kid : n.kids) {
        warp_gj_val v;
        warp_gj_eval(c, nodes, kid, &v);
        if(v.null) {
          any_null = true;
        } else if(warp_gj_true(v) != is_and) {
          /* a false operand of AND, a true operand of OR */
          out->null = false;
          out->i = is_and ? 0 : 1;
          return;
        }
      }
      out->null = any_null;
      out->i = is_and ? 1 : 0;
      return;
    }
    case GJN_NOT: {
      warp_gj_val a;
      warp_gj_eval(c, nodes, n.kids[0], &a);
      out->kind = WARP_GJ_INT;
      out->scale = 0;
      out->null = a.null;
      out->i = warp_gj_true(a) ? 0 : 1;
      return;
    }
    case GJN_CASE: {
      const size_t pairs = (n.kids.size() - (n.has_else ? 1 : 0)) / 2;
      for(size_t k = 0; k < pairs; ++k) {
        warp_gj_val w;
        warp_gj_eval(c, nodes, n.kids[2 * k], &w);
        if(warp_gj_true(w)) {
          warp_gj_eval(c, nodes, n.kids[2 * k + 1], out);
          warp_gj_convert(out, n.kind, n.scale);
          return;
        }
      }
      if(n.has_else) {
        warp_gj_eval(c, nodes, n.kids.back(), out);
        warp_gj_convert(out, n.kind, n.scale);
      } else {
        out->null = true;
        out->kind = n.kind;
      }
      return;
    }
    case GJN_LIKE: {
      warp_gj_val a, p;
      warp_gj_eval(c, nodes, n.kids[0], &a);
      warp_gj_eval(c, nodes, n.kids[1], &p);
      out->kind = WARP_GJ_INT;
      out->scale = 0;
      out->null = a.null || p.null;
      if(out->null) return;
      out->i = my_wildcmp(n.cs, a.s, a.s + a.n, p.s, p.s + p.n, '\\', '_', '%') == 0 ? 1 : 0;
      return;
    }
    case GJN_YEAR: {
      warp_gj_val a;
      warp_gj_eval(c, nodes, n.kids[0], &a);
      out->kind = WARP_GJ_INT;
      out->scale = 0;
      out->null = a.null;
      if(a.null) return;
      MYSQL_TIME t;
      TIME_from_longlong_datetime_packed(&t, (longlong)a.i);
      out->i = t.year;
      return;
    }
    default:
      out->null = true;
  }
}

/* ---------------------------------------------------------------------- */
/* the compilation of the items of the query                              */
/* ---------------------------------------------------------------------- */

struct warp_gj_compiler {
  THD *thd;
  warp_pj_plan *pj;
  warp_gj_nodes nodes;
  bool failed = false;

  /* the columns that the aggregation reads: the number of a column in the columns of its table */
  int level_of(const Field *field) const {
    if(field->table == pj->probe) return 0;
    for(int l = 1; l <= pj->nbuild; ++l) {
      if(pj->tables[l].table == field->table) return l;
    }
    return -1;
  }

  static bool field_kind(Field *field, warp_gj_colspec *spec, bool *temporal, const CHARSET_INFO **cs) {
    spec->field = field->field_index();
    spec->nullable = field->is_nullable();
    *temporal = false;
    *cs = nullptr;
    switch(field->real_type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_YEAR:
        spec->kind = WARP_GJ_INT;
        return true;
      case MYSQL_TYPE_LONGLONG:
        if(field->is_flag_set(UNSIGNED_FLAG)) return false;
        spec->kind = WARP_GJ_INT;
        return true;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
        spec->kind = WARP_GJ_INT;
        *temporal = true;
        return true;
      case MYSQL_TYPE_NEWDECIMAL:
        if(field->decimals() > 18 || static_cast<Field_new_decimal *>(field)->precision > 18) return false;
        spec->kind = WARP_GJ_DEC;
        spec->scale = field->decimals();
        return true;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
        spec->kind = WARP_GJ_REAL;
        return true;
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
        if(field->binary() || (field->all_flags() & BLOB_FLAG)) return false;
        spec->kind = WARP_GJ_STR;
        *cs = field->charset();
        return true;
      default:
        return false;
    }
  }

  /* the column of a field: its node, or -1 */
  int column(Field *field) {
    const int level = level_of(field);
    if(level < 0) return -1;
    warp_gj_colspec spec;
    bool temporal;
    const CHARSET_INFO *cs;
    if(!field_kind(field, &spec, &temporal, &cs)) return -1;
    std::vector<warp_gj_colspec> &specs = level == 0 ? pj->gj_probe : pj->tables[level].gj_specs;
    int ci = -1;
    for(size_t k = 0; k < specs.size(); ++k) {
      if(specs[k].field == spec.field) ci = (int)k;
    }
    if(ci < 0) {
      specs.push_back(spec);
      ci = (int)specs.size() - 1;
    }
    warp_gj_node n;
    n.op = GJN_COL;
    n.kind = spec.kind;
    n.scale = spec.scale;
    n.level = level;
    n.ci = ci;
    n.temporal = temporal;
    n.cs = cs;
    nodes.push_back(n);
    return (int)nodes.size() - 1;
  }

  int add(const warp_gj_node &n) {
    nodes.push_back(n);
    return (int)nodes.size() - 1;
  }

  /* a constant item: its value, or -1 */
  int constant(Item *item, const warp_gj_node *other) {
    warp_gj_node n;
    n.op = GJN_CONST;
    if(is_temporal_type(item->data_type())) {
      /* a date that is compared with a column of dates */
      if(other == nullptr || !other->temporal) return -1;
      const uint64_t packed = warp_item_temporal_value(item);
      if(item->null_value) return -1;
      n.kind = WARP_GJ_INT;
      n.c.kind = WARP_GJ_INT;
      n.c.null = false;
      n.c.i = (__int128)packed;
      return add(n);
    }
    switch(item->result_type()) {
      case INT_RESULT: {
        const longlong v = item->val_int();
        if(item->null_value || (item->unsigned_flag && v < 0)) return -1;
        n.kind = WARP_GJ_INT;
        n.c.kind = WARP_GJ_INT;
        n.c.null = false;
        n.c.i = v;
        return add(n);
      }
      case DECIMAL_RESULT: {
        my_decimal buffer;
        my_decimal *value = item->val_decimal(&buffer);
        if(value == nullptr || item->null_value || item->decimals > 18) return -1;
        String text;
        my_decimal2string(E_DEC_FATAL_ERROR, value, 0, item->decimals, &text);
        int64_t scaled;
        if(!warp_gj_parse_decimal(std::string(text.ptr(), text.length()), item->decimals, &scaled)) return -1;
        n.kind = WARP_GJ_DEC;
        n.scale = item->decimals;
        n.c.kind = WARP_GJ_DEC;
        n.c.null = false;
        n.c.scale = n.scale;
        n.c.i = scaled;
        return add(n);
      }
      case REAL_RESULT: {
        const double v = item->val_real();
        if(item->null_value || std::isnan(v) || std::isinf(v)) return -1;
        n.kind = WARP_GJ_REAL;
        n.c.kind = WARP_GJ_REAL;
        n.c.null = false;
        n.c.d = v;
        return add(n);
      }
      case STRING_RESULT: {
        String buffer;
        String *value = item->val_str(&buffer);
        if(value == nullptr || item->null_value) return -1;
        n.kind = WARP_GJ_STR;
        n.c.kind = WARP_GJ_STR;
        n.c.null = false;
        n.cstr.assign(value->ptr(), value->length());
        n.cs = value->charset();
        return add(n);
      }
      default:
        return -1;
    }
  }

  /* a string constant in the character set of the column that it is compared with */
  bool string_in(int id, const CHARSET_INFO *cs) {
    warp_gj_node &n = nodes[id];
    if(n.op != GJN_CONST || n.kind != WARP_GJ_STR) return true;
    if(n.cs == cs || my_charset_same(n.cs, cs)) return true;
    String converted;
    uint errors = 0;
    if(converted.copy(n.cstr.data(), n.cstr.size(), n.cs, cs, &errors) || errors != 0) return false;
    n.cstr.assign(converted.ptr(), converted.length());
    n.cs = cs;
    return true;
  }

  int compare(Item_func *func, int cmp, Item *left, Item *right) {
    const bool left_const = left->const_item() && left->type() != Item::FIELD_ITEM;
    const bool right_const = right->const_item() && right->type() != Item::FIELD_ITEM;
    int a = -1, b = -1;
    if(left_const && !right_const) {
      b = compile(right);
      if(b < 0) return -1;
      a = constant(left, &nodes[b]);
    } else if(right_const && !left_const) {
      a = compile(left);
      if(a < 0) return -1;
      b = constant(right, &nodes[a]);
    } else {
      a = compile(left);
      b = a < 0 ? -1 : compile(right);
    }
    if(a < 0 || b < 0) return -1;
    warp_gj_node n;
    n.op = GJN_CMP;
    n.kind = WARP_GJ_INT;
    n.cmp = cmp;
    n.kids = {a, b};
    const int ka = nodes[a].kind, kb = nodes[b].kind;
    if(ka == WARP_GJ_STR || kb == WARP_GJ_STR) {
      if(ka != kb) return -1;
      const CHARSET_INFO *cs = func->compare_collation();
      /* the columns are compared in their own collation, which has to be the one of the comparison */
      for(int k : {a, b}) {
        if(nodes[k].op == GJN_COL && nodes[k].cs != cs) return -1;
        if(nodes[k].op != GJN_COL && nodes[k].op != GJN_CONST) return -1;
      }
      if(!string_in(a, cs) || !string_in(b, cs)) return -1;
      n.ck = WARP_GJ_STR;
      n.cs = cs;
    } else if(ka == WARP_GJ_REAL || kb == WARP_GJ_REAL) {
      n.ck = WARP_GJ_REAL;
    } else {
      n.ck = WARP_GJ_DEC;
    }
    return add(n);
  }

  int compile(Item *item) {
    if(failed) return -1;
    Item *real = item->real_item();
    if(real->type() == Item::FIELD_ITEM) {
      Field *field = down_cast<Item_field *>(real)->field;
      return field == nullptr ? -1 : column(field);
    }
    if(item->const_item() && !item->has_subquery() && !item->has_stored_program() && !item->is_non_deterministic()) {
      return constant(item, nullptr);
    }
    if(real->type() == Item::COND_ITEM) {
      Item_cond *cond = down_cast<Item_cond *>(real);
      warp_gj_node n;
      n.kind = WARP_GJ_INT;
      if(cond->functype() == Item_func::COND_AND_FUNC) n.op = GJN_AND;
      else if(cond->functype() == Item_func::COND_OR_FUNC) n.op = GJN_OR;
      else return -1;
      List_iterator_fast<Item> it(*cond->argument_list());
      for(Item *part = it++; part != nullptr; part = it++) {
        const int kid = compile(part);
        if(kid < 0) return -1;
        n.kids.push_back(kid);
      }
      return add(n);
    }
    if(real->type() != Item::FUNC_ITEM) return -1;
    Item_func *func = down_cast<Item_func *>(real);
    switch(func->functype()) {
      case Item_func::EQ_FUNC: return compare(func, GJC_EQ, func->arguments()[0], func->arguments()[1]);
      case Item_func::NE_FUNC: return compare(func, GJC_NE, func->arguments()[0], func->arguments()[1]);
      case Item_func::LT_FUNC: return compare(func, GJC_LT, func->arguments()[0], func->arguments()[1]);
      case Item_func::LE_FUNC: return compare(func, GJC_LE, func->arguments()[0], func->arguments()[1]);
      case Item_func::GT_FUNC: return compare(func, GJC_GT, func->arguments()[0], func->arguments()[1]);
      case Item_func::GE_FUNC: return compare(func, GJC_GE, func->arguments()[0], func->arguments()[1]);
      case Item_func::NOT_FUNC: {
        const int kid = compile(func->arguments()[0]);
        if(kid < 0) return -1;
        warp_gj_node n;
        n.op = GJN_NOT;
        n.kind = WARP_GJ_INT;
        n.kids = {kid};
        return add(n);
      }
      case Item_func::BETWEEN: {
        /* x BETWEEN a AND b is x >= a AND x <= b */
        Item_func_between *between = down_cast<Item_func_between *>(func);
        const int ge = compare(func, GJC_GE, func->arguments()[0], func->arguments()[1]);
        const int le = ge < 0 ? -1 : compare(func, GJC_LE, func->arguments()[0], func->arguments()[2]);
        if(ge < 0 || le < 0) return -1;
        warp_gj_node n;
        n.op = GJN_AND;
        n.kind = WARP_GJ_INT;
        n.kids = {ge, le};
        int result = add(n);
        if(between->negated) {
          warp_gj_node neg;
          neg.op = GJN_NOT;
          neg.kind = WARP_GJ_INT;
          neg.kids = {result};
          result = add(neg);
        }
        return result;
      }
      case Item_func::IN_FUNC: {
        /* x IN (a, b, ...) is x = a OR x = b OR ... */
        Item_func_in *in = down_cast<Item_func_in *>(func);
        warp_gj_node n;
        n.op = GJN_OR;
        n.kind = WARP_GJ_INT;
        for(uint k = 1; k < func->arg_count; ++k) {
          const int eq = compare(func, GJC_EQ, func->arguments()[0], func->arguments()[k]);
          if(eq < 0) return -1;
          n.kids.push_back(eq);
        }
        int result = add(n);
        if(in->negated) {
          warp_gj_node neg;
          neg.op = GJN_NOT;
          neg.kind = WARP_GJ_INT;
          neg.kids = {result};
          result = add(neg);
        }
        return result;
      }
      case Item_func::LIKE_FUNC: {
        if(func->arg_count != 2 || (thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES)) return -1;
        const int subject = compile(func->arguments()[0]);
        if(subject < 0 || nodes[subject].kind != WARP_GJ_STR) return -1;
        const int pattern = compile(func->arguments()[1]);
        if(pattern < 0 || nodes[pattern].kind != WARP_GJ_STR) return -1;
        const CHARSET_INFO *cs = func->compare_collation();
        if(nodes[subject].op != GJN_COL || nodes[subject].cs != cs || !string_in(pattern, cs)) return -1;
        warp_gj_node n;
        n.op = GJN_LIKE;
        n.kind = WARP_GJ_INT;
        n.kids = {subject, pattern};
        n.cs = cs;
        return add(n);
      }
      case Item_func::CASE_FUNC: {
        Item_func_case *c = down_cast<Item_func_case *>(func);
        if(c->get_first_expr_num() != -1) return -1;   // CASE x WHEN ...
        warp_gj_node n;
        n.op = GJN_CASE;
        n.has_else = c->get_else_expr_num() != -1;
        const uint values = func->arg_count - (n.has_else ? 1 : 0);
        for(uint k = 0; k < func->arg_count; ++k) {
          const int kid = compile(func->arguments()[k]);
          if(kid < 0) return -1;
          n.kids.push_back(kid);
        }
        /* the kind of the values: the THEN and ELSE expressions */
        bool real_values = false, string_values = false;
        int scale = 0;
        for(uint k = 0; k < func->arg_count; ++k) {
          const bool value = (k % 2 == 1 && k < values) || (n.has_else && k == func->arg_count - 1);
          if(!value) continue;
          const warp_gj_node &kn = nodes[n.kids[k]];
          if(kn.kind == WARP_GJ_REAL) real_values = true;
          else if(kn.kind == WARP_GJ_STR) string_values = true;
          else scale = std::max(scale, kn.scale);
        }
        if(string_values) {
          for(uint k = 0; k < func->arg_count; ++k) {
            const bool value = (k % 2 == 1 && k < values) || (n.has_else && k == func->arg_count - 1);
            if(value && nodes[n.kids[k]].kind != WARP_GJ_STR) return -1;
          }
          return -1;   // the result of a CASE of strings is left to MySQL
        }
        n.kind = real_values ? WARP_GJ_REAL : WARP_GJ_DEC;
        n.scale = real_values ? 0 : scale;
        return add(n);
      }
      case Item_func::EXTRACT_FUNC: {
        if(down_cast<Item_extract *>(func)->int_type != INTERVAL_YEAR) return -1;
        const int kid = compile(func->arguments()[0]);
        if(kid < 0 || !nodes[kid].temporal) return -1;
        warp_gj_node n;
        n.op = GJN_YEAR;
        n.kind = WARP_GJ_INT;
        n.kids = {kid};
        return add(n);
      }
      case Item_func::YEAR_FUNC: {
        const int kid = compile(func->arguments()[0]);
        if(kid < 0 || !nodes[kid].temporal) return -1;
        warp_gj_node n;
        n.op = GJN_YEAR;
        n.kind = WARP_GJ_INT;
        n.kids = {kid};
        return add(n);
      }
      default:
        break;
    }
    /* + - * and unary - */
    Item_func_plus *plus = dynamic_cast<Item_func_plus *>(func);
    Item_func_minus *minus = dynamic_cast<Item_func_minus *>(func);
    Item_func_mul *mul = dynamic_cast<Item_func_mul *>(func);
    Item_func_neg *neg = dynamic_cast<Item_func_neg *>(func);
    if(plus == nullptr && minus == nullptr && mul == nullptr && neg == nullptr) {
      return -1;
    }
    warp_gj_node n;
    for(uint k = 0; k < func->arg_count; ++k) {
      const int kid = compile(func->arguments()[k]);
      if(kid < 0 || nodes[kid].kind == WARP_GJ_STR) return -1;
      n.kids.push_back(kid);
    }
    if(neg != nullptr) {
      n.op = GJN_NEG;
      n.kind = nodes[n.kids[0]].kind;
      n.scale = nodes[n.kids[0]].scale;
      if(n.kind == WARP_GJ_INT) return -1;
    } else {
      n.op = plus != nullptr ? GJN_ADD : minus != nullptr ? GJN_SUB : GJN_MUL;
      const warp_gj_node &x = nodes[n.kids[0]];
      const warp_gj_node &y = nodes[n.kids[1]];
      if(x.kind == WARP_GJ_REAL || y.kind == WARP_GJ_REAL) {
        n.kind = WARP_GJ_REAL;
        if(func->result_type() != REAL_RESULT) return -1;
      } else {
        /* an arithmetic of integers (BIGINT overflows) is left to MySQL */
        if(func->result_type() != DECIMAL_RESULT) return -1;
        n.kind = WARP_GJ_DEC;
        n.scale = n.op == GJN_MUL ? x.scale + y.scale : std::max(x.scale, y.scale);
        /* the result has the scale and no more digits than the engine holds */
        if((uint)n.scale != func->decimals || n.scale > 18 || func->max_length > 36) return -1;
      }
    }
    return add(n);
  }
};

/* ---------------------------------------------------------------------- */
/* the groups                                                             */
/* ---------------------------------------------------------------------- */

struct warp_gja_agg {
  warp_pa_agg::Kind kind = warp_pa_agg::COUNT_ALL;
  int node = -1;
  int value_kind = WARP_GJ_INT;
};

struct warp_gja_out {
  int node = -1;                  // the column
};

struct warp_gja_sink : warp_pa_sink_base {
  warp_gj_nodes nodes;
  std::vector<int> keys;          // the nodes that the groups are made by
  std::vector<int> filters;       // the conditions that a row has to pass
  std::vector<warp_gja_agg> aggs;
  std::vector<int> outs;          // the columns that MySQL reads above the aggregation, as they are in the first row of a group

  struct Local {
    std::vector<warp_pa_worker> w;
    std::string key;
    std::string collation_key;
    std::vector<warp_pa_cell> cells;
  };

  void *begin() override {
    Local *l = new Local;
    l->w.resize(1);
    l->cells.resize(outs.size());
    return l;
  }

  void add(void *local, const warp_pj_prober &p) override {
    Local &l = *static_cast<Local *>(local);
    warp_gj_ctx ctx{&*pa->pj, static_cast<const std::vector<warp_gj_arr> *>(p.extra), p.index, p.chosen};
    warp_gj_val v;
    for(int f : filters) {
      warp_gj_eval(ctx, nodes, f, &v);
      if(!warp_gj_true(v)) return;
    }
    /* the group */
    l.key.clear();
    for(int k : keys) {
      warp_gj_eval(ctx, nodes, k, &v);
      if(v.null) {
        l.key.push_back('\1');
        continue;
      }
      l.key.push_back('\0');
      switch(v.kind) {
        case WARP_GJ_REAL: {
          double d = v.d == 0 ? 0 : v.d;
          l.key.append((const char *)&d, sizeof(d));
          break;
        }
        case WARP_GJ_STR: {
          const warp_gj_node &kn = nodes[k];
          const std::string s(v.s, v.n);
          warp_pa_collation_key(kn.cs, s, &l.collation_key);
          const uint32_t length = (uint32_t)l.collation_key.size();
          l.key.append((const char *)&length, sizeof(length));
          l.key.append(l.collation_key);
          break;
        }
        default: {
          const int64_t i = (int64_t)v.i;
          l.key.append((const char *)&i, sizeof(i));
        }
      }
    }
    bool added;
    warp_pa_group *g = l.w[0].groups.find_or_add(l.key, &added);
    if(added) {
      g->states.resize(aggs.size());
      for(size_t k = 0; k < outs.size(); ++k) {
        warp_gj_eval(ctx, nodes, outs[k], &v);
        warp_pa_cell &cell = g->cells.emplace_back();
        cell.null = v.null;
        if(v.null) continue;
        switch(v.kind) {
          case WARP_GJ_REAL: cell.d = v.d; break;
          case WARP_GJ_STR: cell.s.assign(v.s, v.n); break;
          default: cell.i = (int64_t)v.i;
        }
      }
    }
    ++g->rows;
    for(size_t m = 0; m < aggs.size(); ++m) {
      const warp_gja_agg &a = aggs[m];
      if(a.kind == warp_pa_agg::COUNT_ALL) continue;
      warp_pa_state &st = g->states[m];
      warp_gj_eval(ctx, nodes, a.node, &v);
      if(v.null) continue;
      switch(a.kind) {
        case warp_pa_agg::COUNT_COL:
          ++st.count;
          break;
        case warp_pa_agg::SUM:
        case warp_pa_agg::AVG:
          ++st.count;
          if(a.value_kind == WARP_GJ_REAL) st.dsum += v.d;
          else st.isum += v.i;
          break;
        default: {   // MIN, MAX
          const bool is_min = a.kind == warp_pa_agg::MIN;
          ++st.count;
          if(a.value_kind == WARP_GJ_STR) {
            const std::string s(v.s, v.n);
            warp_pa_collation_key(nodes[a.node].cs, s, &l.collation_key);
            if(!st.have || (is_min ? l.collation_key < st.skey : l.collation_key > st.skey)) {
              st.have = true;
              st.skey = l.collation_key;
              st.sval = s;
            }
          } else if(a.value_kind == WARP_GJ_REAL) {
            if(!st.have || (is_min ? v.d < st.dval : v.d > st.dval)) {
              st.have = true;
              st.dval = v.d;
            }
          } else if(!st.have || (is_min ? v.i < st.ival : v.i > st.ival)) {
            st.have = true;
            st.ival = v.i;
          }
        }
      }
    }
  }

  void end(void *local) override {
    Local *l = static_cast<Local *>(local);
    try {
      warp_pa_merge_into(*pa, shared.get(), &l->w);
    } catch(...) {
      failed = true;
    }
    delete l;
  }
};

/* ---------------------------------------------------------------------- */
/* the plan                                                               */
/* ---------------------------------------------------------------------- */

/* The plan for the aggregation agg_path of a query block whose rows come from
   the join that warp_pj_replace made (the chain of nested loops that slot is
   the top of, with the conditions that MySQL still evaluates on top of it), or
   none. */
static std::shared_ptr<warp_pa_plan> warp_gja_make_plan(THD *thd, AccessPath *agg_path, AccessPath **slot,
                                                        const JOIN *join, AccessPath *stream, bool sorted) {
  std::shared_ptr<warp_pa_plan> none;
  if(stream != nullptr) {
    return (warp_pa_no(__LINE__), none);
  }
  /* the conditions that MySQL evaluates for the rows of the join, and the chain of the join */
  AccessPath *node = *slot;
  std::vector<Item *> conditions;
  while(node->type == AccessPath::FILTER) {
    conditions.push_back(node->filter().condition);
    node = node->filter().child;
  }
  std::vector<TABLE *> followers;
  while(node->type == AccessPath::NESTED_LOOP_JOIN) {
    AccessPath *inner = node->nested_loop_join().inner;
    if(inner->type != AccessPath::TABLE_SCAN) {
      return (warp_pa_no(__LINE__), none);
    }
    followers.push_back(inner->table_scan().table);
    node = node->nested_loop_join().outer;
  }
  if(node->type != AccessPath::TABLE_SCAN || followers.empty()) {
    return (warp_pa_no(__LINE__), none);
  }
  TABLE *probe = node->table_scan().table;
  ha_warp *handler = probe == nullptr ? nullptr : dynamic_cast<ha_warp *>(probe->file);
  if(handler == nullptr || !handler->par_join || handler->par_agg || handler->star_agg) {
    return (warp_pa_no(__LINE__), none);
  }
  std::shared_ptr<warp_pj_plan> pj = handler->par_join;
  if(pj->probe != probe || pj->sink || (int)followers.size() != pj->nbuild) {
    return (warp_pa_no(__LINE__), none);
  }
  for(int l = 1; l <= pj->nbuild; ++l) {
    bool found = false;
    for(TABLE *t : followers) found = found || t == pj->tables[l].table;
    if(!found || !pj->tables[l].filters.empty() || !pj->tables[l].semi.empty()) return (warp_pa_no(__LINE__), none);
  }
  /* the sets of columns of the other joins of the engine are not used by this one */
  if(!pj->gj_probe.empty()) {
    return (warp_pa_no(__LINE__), none);
  }

  auto plan = std::make_shared<warp_pa_plan>();
  plan->thd = thd;
  plan->table = probe;
  plan->handler = handler;
  auto sink = std::make_shared<warp_gja_sink>();
  warp_gj_compiler compiler{thd, pj.get()};
  auto undo = [&]() {
    pj->gj_probe.clear();
    for(int l = 1; l <= pj->nbuild; ++l) pj->tables[l].gj_specs.clear();
  };

  /* GROUP BY */
  ORDER *group = nullptr;
  if(agg_path->type == AccessPath::TEMPTABLE_AGGREGATE) {
    group = agg_path->temptable_aggregate().table->group;
  } else {
    if(agg_path->aggregate().olap != UNSPECIFIED_OLAP_TYPE) {
      return (warp_pa_no(__LINE__), none);
    }
    group = join->group_list.order;
    if(group != nullptr && !sorted) {
      return (warp_pa_no(__LINE__), none);
    }
  }
  std::vector<Item *> group_items;
  for(; group != nullptr; group = group->next) {
    group_items.push_back(*group->item);
    const int key = compiler.compile(*group->item);
    if(key < 0) {
      undo();
      return (warp_pa_no(__LINE__), none);
    }
    sink->keys.push_back(key);
  }
  plan->group_cols = sink->keys.size();

  /* the conditions that MySQL evaluates */
  for(Item *condition : conditions) {
    const int f = compiler.compile(condition);
    if(f < 0) {
      undo();
      return (warp_pa_no(__LINE__), none);
    }
    sink->filters.push_back(f);
  }

  /* the aggregates */
  if(join->sum_funcs == nullptr || *join->sum_funcs == nullptr) {
    undo();
    return (warp_pa_no(__LINE__), none);
  }
  for(Item_sum **it = join->sum_funcs; *it != nullptr; ++it) {
    Item_sum *sum = *it;
    warp_pa_agg agg;
    warp_gja_agg g;
    agg.item = sum;
    agg.replaced = true;
    if(sum->has_with_distinct() || sum->argument_count() != 1) {
      undo();
      return (warp_pa_no(__LINE__), none);
    }
    Item *arg = sum->get_arg(0);
    const Item_sum::Sumfunctype type = sum->sum_func();
    if(type == Item_sum::COUNT_FUNC && (arg->type() == Item::INT_ITEM || arg->const_item())) {
      agg.kind = warp_pa_agg::COUNT_ALL;
    } else {
      g.node = compiler.compile(arg);
      if(g.node < 0) {
        undo();
        return (warp_pa_no(__LINE__), none);
      }
      const warp_gj_node &an = compiler.nodes[g.node];
      g.value_kind = an.kind;
      switch(type) {
        case Item_sum::COUNT_FUNC:
          agg.kind = warp_pa_agg::COUNT_COL;
          break;
        case Item_sum::SUM_FUNC:
        case Item_sum::AVG_FUNC:
          if(an.kind == WARP_GJ_STR) {
            undo();
            return (warp_pa_no(__LINE__), none);
          }
          agg.kind = type == Item_sum::SUM_FUNC ? warp_pa_agg::SUM : warp_pa_agg::AVG;
          if(an.kind == WARP_GJ_REAL) {
            if(sum->result_type() != REAL_RESULT) {
              undo();
              return (warp_pa_no(__LINE__), none);
            }
            agg.result_kind = PA_REAL;
          } else {
            if(sum->result_type() != DECIMAL_RESULT || (uint)an.scale != arg->decimals) {
              undo();
              return (warp_pa_no(__LINE__), none);
            }
            agg.result_kind = PA_DEC;
            agg.result_scale = an.scale;
          }
          break;
        case Item_sum::MIN_FUNC:
        case Item_sum::MAX_FUNC:
          agg.kind = type == Item_sum::MIN_FUNC ? warp_pa_agg::MIN : warp_pa_agg::MAX;
          if(an.kind == WARP_GJ_STR) {
            if(an.op != GJN_COL || an.cs != sum->collation.collation) {
              undo();
              return (warp_pa_no(__LINE__), none);
            }
            agg.result_kind = PA_STR;
          } else if(an.kind == WARP_GJ_REAL) {
            if(sum->result_type() != REAL_RESULT) {
              undo();
              return (warp_pa_no(__LINE__), none);
            }
            agg.result_kind = PA_REAL;
          } else if(sum->result_type() == DECIMAL_RESULT) {
            agg.result_kind = PA_DEC;
            agg.result_scale = an.scale;
          } else if(sum->result_type() == INT_RESULT && !an.temporal) {
            agg.result_kind = PA_INT;
          } else {
            undo();
            return (warp_pa_no(__LINE__), none);
          }
          break;
        default:
          undo();
          return (warp_pa_no(__LINE__), none);
      }
    }
    g.kind = agg.kind;
    sink->aggs.push_back(g);
    plan->aggs.push_back(agg);
  }

  /* the columns that MySQL reads above the aggregation: in the group by and in the select list
     (outside the aggregates), they have the values of the first row of the group */
  std::vector<Field *> out_fields;
  bool bad = false;
  auto collect = [&](Item *item) {
    WalkItem(item, enum_walk::PREFIX, [&](Item *i) {
      if(i->type() == Item::SUM_FUNC_ITEM) {
        return true;
      }
      if(i->type() == Item::FIELD_ITEM) {
        Field *field = down_cast<Item_field *>(i)->field;
        if(field == nullptr || compiler.level_of(field) < 0) {
          bad = true;
          return false;
        }
        if(std::find(out_fields.begin(), out_fields.end(), field) == out_fields.end()) out_fields.push_back(field);
      } else if(i->has_subquery() || i->has_stored_program()) {
        bad = true;
      }
      return false;
    });
  };
  for(Item *item : group_items) collect(item);
  /* the select list of the block, not the one that points to the temporary table */
  for(Item *item : join->query_block->fields) collect(item);
  if(bad || join->having_cond != nullptr) {
    undo();
    return (warp_pa_no(__LINE__), none);
  }
  for(Field *field : out_fields) {
    const int node = compiler.column(field);
    if(node < 0) {
      undo();
      return (warp_pa_no(__LINE__), none);
    }
    sink->outs.push_back(node);
    warp_pa_out out;
    out.table = field->table;
    out.field = field->field_index();
    out.kind = compiler.nodes[node].kind;
    out.scale = compiler.nodes[node].scale;
    out.cs = field->charset();
    out.f = field;
    plan->outs.push_back(out);
  }

  /* no image of the rows of the tables is kept, nothing is read by MySQL */
  for(int l = 1; l <= pj->nbuild; ++l) {
    warp_pj_table &t = pj->tables[l];
    t.ranges.clear();
    t.image_len = 0;
  }
  for(const warp_pa_out &out : plan->outs) {
    bitmap_set_bit(out.table->read_set, out.field);
  }
  sink->nodes = std::move(compiler.nodes);
  sink->pa = plan.get();
  plan->adjacent = sorted;
  plan->pj = pj;
  pj->sink = sink;
  return plan;
}

static bool warp_gja_replace(THD *thd, AccessPath *agg, const JOIN *join) {
  AccessPath **const top_slot = agg->type == AccessPath::AGGREGATE ? &agg->aggregate().child
                                                                   : &agg->temptable_aggregate().subquery_path;
  AccessPath **slot = top_slot;
  bool sorted = false;
  while((*slot)->type == AccessPath::SORT) {
    sorted = true;
    slot = &(*slot)->sort().child;
  }
  if(getenv("WARP_PA_TRACE") != nullptr) {
    sql_print_warning("WARP: aggregation of the rows of a join: trying");
  }
  std::shared_ptr<warp_pa_plan> plan = warp_gja_make_plan(thd, agg, slot, join, nullptr, sorted);
  if(!plan) {
    return false;
  }
  /* the tables are read by the engine, not by the scans below the aggregation */
  std::shared_ptr<warp_pj_plan> pj = plan->pj;
  for(int l = 1; l <= pj->nbuild; ++l) {
    pj->tables[l].handler->pj_follower.reset();
    pj->tables[l].handler->pj_follower_pending = false;
  }
  pj->probe_handler->par_join.reset();
  /* the chain of the join and its conditions are replaced */
  AccessPath *chain = *slot;
  while(chain->type == AccessPath::FILTER) chain = chain->filter().child;
  return warp_pa_install(thd, plan, slot, top_slot, nullptr);
}

#endif
