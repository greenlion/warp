/*
  Parallel aggregation of one WARP table inside the engine.

  MySQL aggregates the rows that a table scan returns one at a time in one
  thread: for the large table of a query such as TPC-H Q1

    SELECT l_returnflag, l_linestatus, SUM(l_quantity), SUM(l_extendedprice),
           SUM(l_extendedprice * (1 - l_discount)), AVG(l_discount), COUNT(*)
      FROM lineitem WHERE l_shipdate <= ... GROUP BY l_returnflag, l_linestatus

  it reads six million rows through the handler, which converts every value
  into the field of a record, only to add it to a few sums.  Here the engine
  does it:

   * The partitions of the table (and slices of the rows of a partition) are
     processed by worker threads: the pushed down conditions are evaluated with
     the bitmap indexes, the rows that the transaction can see are grouped by
     the columns of the GROUP BY (integers, decimals, floating point numbers
     and strings; strings by the key of their collation) and the aggregates are
     added: SUM and AVG of integer, decimal and floating point expressions,
     COUNT, MIN and MAX of numbers and strings.
   * The groups of the workers are merged.
   * The scan of the table returns one row per group.  The columns of the GROUP
     BY are stored in the fields of the table, the aggregates are given to
     MySQL through constants that replace their arguments (SUM, MIN, MAX), or
     through the classes that take the sum and the count of the rows that a
     group stands for (COUNT, AVG).  MySQL aggregates the rows again (every
     group is one row), applies HAVING, ORDER BY and LIMIT.
   * COUNT(DISTINCT), SUM(DISTINCT) and AVG(DISTINCT) are not replaced: the
     groups are split by the values of the columns that the aggregate reads
     and MySQL de-duplicates them (those columns are stored in the fields of
     the table too).

  The query is only changed when the engine can do exactly what MySQL would:
  see warp_pa_make_plan for what is accepted.  This file is included by
  ha_warp.cc, the plugin is a single translation unit.
*/

#ifndef WARP_PAR_AGG_H
#define WARP_PAR_AGG_H

#include <cmath>
#include <thread>
#include <functional>
#include <deque>
#include <string_view>
#include <unordered_map>

#include "mysql/strings/m_ctype.h"

static inline bool warp_pa_no(int line) {
  if(getenv("WARP_PA_TRACE") != nullptr) {
    sql_print_warning("WARP: parallel aggregation not used, warp_par_agg.h:%d", line);
  }
  return false;
}

/* the kinds of values that the engine aggregates */
enum warp_pa_kind { PA_INT, PA_DEC, PA_REAL, PA_STR };

/* a column of the table that is read: a measure, a column of the GROUP BY, or
   a column that a DISTINCT aggregate reads */
struct warp_pa_col {
  uint field = 0;
  int kind = PA_INT;
  bool is_unsigned = false;
  int scale = 0;                       // PA_DEC
  bool nullable = false;
  const CHARSET_INFO *cs = nullptr;    // PA_STR
  Field *f = nullptr;
};

/* A number of an expression: the program is a postfix form evaluated in
   __int128 for integers and decimals (with the scale of the result known when
   the program is made) or in double if a floating point number takes part. */
struct warp_pa_op {
  enum Kind { COLUMN, CONSTANT, ADD, SUB, MUL } kind;
  int slot = 0;                // COLUMN: position in warp_pa_plan::cols
  warp_i128 value = 0;         // CONSTANT (integer program)
  double dvalue = 0;           // CONSTANT (real program)
  int shift_a = 0;             // ADD and SUB: the operands are multiplied by 10^shift
  int shift_b = 0;
};

struct warp_pa_prog {
  bool real = false;           // evaluated in double
  int scale = 0;               // integer program: the scale of the result
  std::vector<warp_pa_op> ops;
  std::vector<int> cols;       // columns the program reads (positions in warp_pa_plan::cols)
  std::string text;
};

/* a constant that holds a string of the engine */
class warp_pa_string : public Item_string {
 public:
  warp_pa_string(const char *str, size_t length, const CHARSET_INFO *cs) : Item_string(str, length, cs) {}
  void assign(const std::string &value, bool is_null) {
    str_value.copy(value.data(), value.size(), str_value.charset());
    null_value = is_null;
  }
};

/* one aggregate of the query */
struct warp_pa_agg {
  enum Kind { COUNT_ALL, COUNT_COL, SUM, AVG, MIN, MAX } kind = COUNT_ALL;
  Item_sum *item = nullptr;
  bool replaced = false;       // the argument is given by the engine (not a DISTINCT aggregate)
  warp_pa_prog prog;           // SUM, AVG, MIN, MAX and COUNT of a number
  int str_col = -1;            // MIN, MAX, COUNT of a string column: position in warp_pa_plan::cols
  int result_kind = PA_INT;    // PA_INT, PA_DEC, PA_REAL, PA_STR: the kind of the sum or of the extreme value
  int result_scale = 0;        // PA_DEC
  /* the constants that replace the argument */
  Item_decimal *dec_holder = nullptr;
  Item_int *int_holder = nullptr;
  Item_float *real_holder = nullptr;
  warp_pa_string *str_holder = nullptr;
};

/* what has been added for one aggregate of one group */
struct warp_pa_state {
  warp_i128 isum = 0;
  double dsum = 0;
  uint64_t count = 0;          // values that were not NULL
  bool have = false;           // MIN, MAX: a value was seen
  warp_i128 ival = 0;
  double dval = 0;
  std::string sval;            // MIN, MAX of a string: the string
  std::string skey;            // and its key in the collation
};

/* a value of a column of the group (or of the DISTINCT columns) */
struct warp_pa_cell {
  bool null = false;
  int64_t i = 0;
  double d = 0;
  std::string s;
};

struct warp_pa_group {
  std::vector<warp_pa_cell> cells;
  std::vector<warp_pa_state> states;
  uint64_t rows = 0;
};

/* A condition of the HAVING clause that the engine checks for a group:
   aggregate OP constant */
struct warp_pa_having {
  size_t agg = 0;
  Item_func::Functype op = Item_func::EQ_FUNC;   // the aggregate is on the left of the operator
  warp_i128 ivalue = 0;                          // integer and decimal aggregates: the constant ...
  int iscale = 0;                                // ... and its scale
  double dvalue = 0;                             // floating point aggregates
};

/* A column of a table that MySQL reads above the aggregation of the rows of a
   join, written to the record of its table from the values of the group (see
   warp_par_gja.h) */
struct warp_pa_out {
  TABLE *table = nullptr;
  uint field = 0;
  int kind = 0;               // WARP_GJ_*
  int scale = 0;
  const CHARSET_INFO *cs = nullptr;
  Field *f = nullptr;
};

struct warp_pa_plan {
  THD *thd = nullptr;
  TABLE *table = nullptr;
  ha_warp *handler = nullptr;
  std::vector<warp_pa_col> cols;        // every column that is read
  std::vector<int> key_cols;            // the columns of the groups: GROUP BY, then the DISTINCT columns
  size_t group_cols = 0;                // the first group_cols of key_cols are the GROUP BY columns
  std::vector<warp_pa_agg> aggs;
  std::vector<warp_pa_having> having;
  /* The groups with the same GROUP BY columns are next to each other: the
     aggregation of MySQL that reads them is not preceded by a sort */
  bool adjacent = false;
  std::string where;                    // the conditions that were pushed down
  std::vector<uint> where_fields;
  std::atomic<bool> overflow{false};

  /* The keys of another table (see warp_try_semi_keys): only the rows whose
     column has one of them are grouped.  A bit per value from semi_min. */
  int semi_slot = -1;                 // the column of the keys, in cols
  bool semi_active = false;
  int64_t semi_min = 0;
  uint64_t semi_range = 0;
  std::vector<uint64_t> semi_bits;
  bool semi_has(int64_t key) const {
    const uint64_t i = (uint64_t)(key - semi_min);
    return i < semi_range && ((semi_bits[i >> 6] >> (i & 63)) & 1);
  }

  /* the result */
  std::vector<warp_pa_group> groups;
  /* the rows come from the join of the engine (warp_pj_plan::sink) */
  std::shared_ptr<warp_pj_plan> pj;
  std::vector<warp_pa_out> outs;       // the columns of the tables of the join that the groups write
  size_t pos = 0;
  bool computed = false;
  /* NULL when the rows of the group have no value: the aggregates of the row being returned */
  ~warp_pa_plan();
};

/* The row of the engine that MySQL is aggregating, for the classes of COUNT and
   AVG */
static thread_local warp_pa_plan *warp_pa_now_plan = nullptr;
static thread_local const warp_pa_group *warp_pa_now_group = nullptr;

inline warp_pa_plan::~warp_pa_plan() {
  if(warp_pa_now_plan == this) {
    warp_pa_now_plan = nullptr;
    warp_pa_now_group = nullptr;
  }
}

static bool warp_pa_count_of(const Item *item, longlong *count) {
  if(warp_pa_now_plan == nullptr || warp_pa_now_group == nullptr) {
    return false;
  }
  for(size_t m = 0; m < warp_pa_now_plan->aggs.size(); ++m) {
    if(warp_pa_now_plan->aggs[m].item == item) {
      const warp_pa_agg &agg = warp_pa_now_plan->aggs[m];
      const warp_pa_state &st = warp_pa_now_group->states[m];
      *count = (longlong)(agg.kind == warp_pa_agg::COUNT_ALL ? warp_pa_now_group->rows : st.count);
      return true;
    }
  }
  return false;
}

/* COUNT of a row of the engine: the row stands for many rows, MySQL adds the
   number of them (the same trick as the star aggregation) */
struct warp_pa_count_tag {
  typedef longlong Item_sum_count::*type;
  friend type warp_pa_count_member(warp_pa_count_tag);
};
template <typename Tag, typename Tag::type Member>
struct warp_pa_count_rob {
  friend typename Tag::type warp_pa_count_member(Tag) { return Member; }
};
template struct warp_pa_count_rob<warp_pa_count_tag, &Item_sum_count::count>;

class warp_pa_count final : public Item_sum_count {
 public:
  warp_pa_count(THD *thd, Item_sum_count *item) : Item_sum_count(thd, item) {}

 protected:
  bool add() override {
    longlong n;
    if(!warp_pa_count_of(this, &n)) {
      n = 1;
    }
    this->*warp_pa_count_member(warp_pa_count_tag()) += n;
    return false;
  }
  void reset_field() override {
    longlong n;
    if(!warp_pa_count_of(this, &n)) {
      Item_sum_count::reset_field();
      return;
    }
    int8store(result_field->field_ptr(), n);
  }
  void update_field() override {
    longlong n;
    if(!warp_pa_count_of(this, &n)) {
      Item_sum_count::update_field();
      return;
    }
    uchar *res = result_field->field_ptr();
    int8store(res, sint8korr(res) + n);
  }
};
static_assert(sizeof(warp_pa_count) == sizeof(Item_sum_count),
              "warp_pa_count must not add members");

static void warp_pa_adopt_count(THD *thd, Item_sum *item) {
  warp_pa_count *spare = new (thd->mem_root) warp_pa_count(thd, down_cast<Item_sum_count *>(item));
  memcpy((void *)item, (const void *)spare, sizeof(void *));  // the virtual table
}

/* AVG of a row of the engine: the argument of the row is the sum of the group
   and the row stands for the number of rows that were added.  Item_sum_avg is
   final: this class has the members of Item_sum_avg and the virtual functions
   of Item_sum_avg (they call the ones of Item_sum_avg) except for the three
   that add: the object of an Item_sum_avg gets the virtual table of this
   class. */
class warp_pa_avg : public Item_sum_sum {
 public:
  uint prec_increment;
  uint f_precision, f_scale, dec_bin_size;
  my_decimal m_avg_dec;
  double m_avg;

  warp_pa_avg(THD *thd, Item_sum_avg *item) : Item_sum_sum(thd, item), prec_increment(item->prec_increment) {}

  Item_sum_avg *as_avg() { return reinterpret_cast<Item_sum_avg *>(this); }

  bool resolve_type(THD *thd) override { return as_avg()->Item_sum_avg::resolve_type(thd); }
  enum Sumfunctype sum_func() const override {
    return has_with_distinct() ? AVG_DISTINCT_FUNC : AVG_FUNC;
  }
  void clear() override { Item_sum_sum::clear(); }
  double val_real() override { return as_avg()->Item_sum_avg::val_real(); }
  longlong val_int() override { return as_avg()->Item_sum_avg::val_int(); }
  my_decimal *val_decimal(my_decimal *d) override { return as_avg()->Item_sum_avg::val_decimal(d); }
  String *val_str(String *str) override { return as_avg()->Item_sum_avg::val_str(str); }
  Item *result_item(Field *field) override { return as_avg()->Item_sum_avg::result_item(field); }
  const char *func_name() const override { return "avg"; }
  Item *copy_or_same(THD *thd) override { return as_avg()->Item_sum_avg::copy_or_same(thd); }
  Field *create_tmp_field(bool group, TABLE *table) override {
    return as_avg()->Item_sum_avg::create_tmp_field(group, table);
  }
  void cleanup() override {
    m_count = 0;
    m_frame_null_count = 0;
    Item_sum_sum::cleanup();
  }

  /* the number of rows that the row of the group stands for */
  longlong rows() {
    longlong n;
    if(!warp_pa_count_of(this, &n)) {
      n = 1;
    }
    return n;
  }
  bool add() override {
    if(Item_sum_sum::add()) return true;
    if(!aggr->arg_is_null(true)) m_count += rows();
    return false;
  }
  void reset_field() override {
    uchar *res = result_field->field_ptr();
    if(hybrid_type == DECIMAL_RESULT) {
      longlong tmp;
      my_decimal value;
      my_decimal *arg_dec = args[0]->val_decimal(&value);
      if(arg_dec == nullptr) {
        value.init();
        arg_dec = &value;
        tmp = 0;
      } else {
        tmp = rows();
      }
      my_decimal2binary(E_DEC_FATAL_ERROR, arg_dec, res, f_precision, f_scale);
      res += dec_bin_size;
      int8store(res, tmp);
    } else {
      const double nr = args[0]->val_real();
      if(args[0]->null_value) {
        memset(res, 0, sizeof(double) + sizeof(longlong));
      } else {
        longlong tmp = rows();
        float8store(res, nr);
        res += sizeof(double);
        int8store(res, tmp);
      }
    }
  }
  void update_field() override {
    longlong field_count;
    uchar *res = result_field->field_ptr();
    if(hybrid_type == DECIMAL_RESULT) {
      my_decimal value;
      my_decimal *arg_val = args[0]->val_decimal(&value);
      if(arg_val == nullptr) {
        return;
      }
      binary2my_decimal(E_DEC_FATAL_ERROR, res, dec_buffs + 1, f_precision, f_scale);
      field_count = sint8korr(res + dec_bin_size);
      my_decimal_add(E_DEC_FATAL_ERROR, dec_buffs, arg_val, dec_buffs + 1);
      my_decimal2binary(E_DEC_FATAL_ERROR, dec_buffs, res, f_precision, f_scale);
      res += dec_bin_size;
      field_count += rows();
      int8store(res, field_count);
    } else {
      const double nr = args[0]->val_real();
      if(!args[0]->null_value) {
        double old_nr = float8get(res);
        field_count = sint8korr(res + sizeof(double));
        old_nr += nr;
        field_count += rows();
        float8store(res, old_nr);
        res += sizeof(double);
        int8store(res, field_count);
      }
    }
  }
};
static_assert(sizeof(warp_pa_avg) == sizeof(Item_sum_avg), "warp_pa_avg must have the members of Item_sum_avg");

static void warp_pa_adopt_avg(THD *thd, Item_sum *item) {
  warp_pa_avg *spare = new (thd->mem_root) warp_pa_avg(thd, down_cast<Item_sum_avg *>(item));
  memcpy((void *)item, (const void *)spare, sizeof(void *));  // the virtual table
}

/* ---------------------------------------------------------------------- */
/* the part of the work that does not depend on the data                  */
/* ---------------------------------------------------------------------- */

static const warp_i128 warp_pa_pow10(int n) {
  warp_i128 v = 1;
  while(n-- > 0) v *= 10;
  return v;
}

/* The kind of the values of a field, false if the engine can not aggregate it */
static bool warp_pa_field_kind(Field *field, warp_pa_col *col) {
  col->field = field->field_index();
  col->nullable = field->is_nullable();
  col->f = field;
  switch(field->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      col->kind = PA_INT;
      col->is_unsigned = field->is_flag_set(UNSIGNED_FLAG);
      return true;
    case MYSQL_TYPE_LONGLONG:
      if(field->is_flag_set(UNSIGNED_FLAG)) {
        return warp_pa_no(__LINE__);
      }
      col->kind = PA_INT;
      return true;
    case MYSQL_TYPE_NEWDECIMAL:
      /* the digits fit a 64 bit integer */
      if(field->decimals() > 18 || static_cast<Field_new_decimal *>(field)->precision > 18) {
        return warp_pa_no(__LINE__);
      }
      col->kind = PA_DEC;
      col->scale = field->decimals();
      col->cs = field->charset();
      return true;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      col->kind = PA_REAL;
      return true;
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
      /* a binary string is stored encoded */
      if(field->binary() || (field->all_flags() & BLOB_FLAG)) {
        return warp_pa_no(__LINE__);
      }
      col->kind = PA_STR;
      col->cs = field->charset();
      return true;
    default:
      return warp_pa_no(__LINE__);
  }
}

static int warp_pa_add_col(warp_pa_plan *plan, Field *field) {
  for(size_t i = 0; i < plan->cols.size(); ++i) {
    if(plan->cols[i].field == field->field_index()) return (int)i;
  }
  warp_pa_col col;
  if(!warp_pa_field_kind(field, &col)) {
    return -1;
  }
  plan->cols.push_back(col);
  return (int)plan->cols.size() - 1;
}

/* Is any floating point number in the expression? */
static bool warp_pa_is_real(Item *item) {
  if(item->type() == Item::FIELD_ITEM) {
    Field *field = down_cast<Item_field *>(item)->field;
    return field != nullptr && (field->real_type() == MYSQL_TYPE_FLOAT || field->real_type() == MYSQL_TYPE_DOUBLE);
  }
  if(item->type() == Item::REAL_ITEM) {
    return true;
  }
  if(item->type() == Item::FUNC_ITEM) {
    Item_func *func = down_cast<Item_func *>(item);
    if(func->result_type() == REAL_RESULT) {
      return true;
    }
    for(uint i = 0; i < func->argument_count(); ++i) {
      if(warp_pa_is_real(func->arguments()[i])) return true;
    }
  }
  return false;
}

/* The program of a numeric expression: columns of the table, constants, + - *.
   Returns the scale of the integer program or the kind of the real one. */
static bool warp_pa_compile(Item *item, warp_pa_plan *plan, warp_pa_prog *prog, int *scale) {
  if(item->type() == Item::FIELD_ITEM) {
    Field *field = down_cast<Item_field *>(item)->field;
    if(field == nullptr || field->table != plan->table) {
      return warp_pa_no(__LINE__);
    }
    const int slot = warp_pa_add_col(plan, field);
    if(slot < 0 || plan->cols[slot].kind == PA_STR) {
      return warp_pa_no(__LINE__);
    }
    if(!prog->real && plan->cols[slot].kind == PA_REAL) {
      return warp_pa_no(__LINE__);
    }
    warp_pa_op op;
    op.kind = warp_pa_op::COLUMN;
    op.slot = slot;
    prog->ops.push_back(op);
    if(std::find(prog->cols.begin(), prog->cols.end(), slot) == prog->cols.end()) {
      prog->cols.push_back(slot);
    }
    *scale = plan->cols[slot].kind == PA_DEC ? plan->cols[slot].scale : 0;
    return true;
  }
  if(item->type() == Item::INT_ITEM || item->type() == Item::DECIMAL_ITEM || item->type() == Item::REAL_ITEM) {
    warp_pa_op op;
    op.kind = warp_pa_op::CONSTANT;
    if(prog->real) {
      op.dvalue = item->val_real();
      if(item->null_value) return warp_pa_no(__LINE__);
      *scale = 0;
    } else if(item->type() == Item::INT_ITEM) {
      if(item->unsigned_flag && (ulonglong)item->val_int() > (ulonglong)INT64_MAX) {
        return warp_pa_no(__LINE__);
      }
      op.value = item->val_int();
      *scale = 0;
    } else if(item->type() == Item::DECIMAL_ITEM) {
      /* the digits of the decimal, as an integer with its scale */
      my_decimal buffer;
      my_decimal *dec = item->val_decimal(&buffer);
      if(dec == nullptr || item->decimals > 18) return warp_pa_no(__LINE__);
      String digits;
      my_decimal2string(E_DEC_FATAL_ERROR, dec, 0, item->decimals, &digits);
      warp_i128 v = 0;
      bool negative = false;
      for(size_t i = 0; i < digits.length(); ++i) {
        const char ch = digits.ptr()[i];
        if(ch == '-') negative = true;
        else if(ch >= '0' && ch <= '9') v = v * 10 + (ch - '0');
      }
      op.value = negative ? -v : v;
      *scale = item->decimals;
    } else {
      return warp_pa_no(__LINE__);
    }
    prog->ops.push_back(op);
    return true;
  }
  if(item->type() == Item::FUNC_ITEM) {
    Item_func *func = down_cast<Item_func *>(item);
    warp_pa_op::Kind kind;
    switch(func->functype()) {
      case Item_func::PLUS_FUNC:  kind = warp_pa_op::ADD; break;
      case Item_func::MINUS_FUNC: kind = warp_pa_op::SUB; break;
      case Item_func::MUL_FUNC:   kind = warp_pa_op::MUL; break;
      default: return warp_pa_no(__LINE__);
    }
    if(func->argument_count() != 2) {
      return warp_pa_no(__LINE__);
    }
    int sa = 0, sb = 0;
    if(!warp_pa_compile(func->arguments()[0], plan, prog, &sa) ||
       !warp_pa_compile(func->arguments()[1], plan, prog, &sb)) {
      return false;
    }
    warp_pa_op op;
    op.kind = kind;
    if(!prog->real) {
      if(kind == warp_pa_op::MUL) {
        *scale = sa + sb;
      } else {
        *scale = std::max(sa, sb);
        op.shift_a = *scale - sa;
        op.shift_b = *scale - sb;
      }
      if(*scale > 30) return warp_pa_no(__LINE__);
    }
    prog->ops.push_back(op);
    return true;
  }
  return warp_pa_no(__LINE__);
}

/* the program of an argument: the kind and the scale of the result must be the
   ones that MySQL gives the argument (its type) */
static bool warp_pa_program(Item *arg, warp_pa_plan *plan, warp_pa_prog *prog) {
  prog->real = warp_pa_is_real(arg);
  int scale = 0;
  if(!warp_pa_compile(arg, plan, prog, &scale)) {
    return false;
  }
  prog->scale = scale;
  if(prog->real) {
    if(arg->result_type() != REAL_RESULT) {
      return warp_pa_no(__LINE__);
    }
  } else if(arg->result_type() == DECIMAL_RESULT) {
    if((int)arg->decimals != scale) {
      return warp_pa_no(__LINE__);
    }
  } else if(arg->result_type() != INT_RESULT) {
    return warp_pa_no(__LINE__);
  }
  String text;
  arg->print(plan->thd, &text, QT_ORDINARY);
  prog->text.assign(text.ptr(), text.length());
  return true;
}

/* The fields of the table that the expression reads: for the DISTINCT
   aggregates, which are not replaced */
static bool warp_pa_fields_of(Item *item, warp_pa_plan *plan, std::vector<int> *cols) {
  if(item->type() == Item::FIELD_ITEM) {
    Field *field = down_cast<Item_field *>(item)->field;
    if(field == nullptr || field->table != plan->table) {
      return warp_pa_no(__LINE__);
    }
    const int slot = warp_pa_add_col(plan, field);
    if(slot < 0) return false;
    if(std::find(cols->begin(), cols->end(), slot) == cols->end()) cols->push_back(slot);
    return true;
  }
  if(item->type() == Item::INT_ITEM || item->type() == Item::DECIMAL_ITEM || item->type() == Item::REAL_ITEM ||
     item->type() == Item::STRING_ITEM) {
    return true;
  }
  if(item->type() == Item::FUNC_ITEM) {
    Item_func *func = down_cast<Item_func *>(item);
    switch(func->functype()) {
      case Item_func::PLUS_FUNC:
      case Item_func::MINUS_FUNC:
      case Item_func::MUL_FUNC:
      case Item_func::DIV_FUNC:
        break;
      default:
        return warp_pa_no(__LINE__);
    }
    for(uint i = 0; i < func->argument_count(); ++i) {
      if(!warp_pa_fields_of(func->arguments()[i], plan, cols)) return false;
    }
    return true;
  }
  return warp_pa_no(__LINE__);
}

static uint64_t warp_pa_rows_of(ha_warp *handler) {
  try {
    warp_table_read_lock lock(handler->get_warp_share()->data_dir_name);
    return warp_table_estimate(handler->get_warp_share()->data_dir_name, "");
  } catch(...) {
    return 0;
  }
}

/* The plan for the aggregation agg_path of a query block, or none.  scan is the
   scan of the table below it. */
static std::shared_ptr<warp_pa_plan> warp_pa_make_plan(THD *thd, AccessPath *agg_path, AccessPath **slot,
                                                       const JOIN *join, AccessPath *stream, bool sorted) {
  std::shared_ptr<warp_pa_plan> none;
  if((*slot)->type != AccessPath::TABLE_SCAN) {
    return (warp_pa_no(__LINE__), none);   // a condition that was not pushed down is a FILTER
  }
  TABLE *table = (*slot)->table_scan().table;
  ha_warp *handler = table == nullptr ? nullptr : dynamic_cast<ha_warp *>(table->file);
  if(handler == nullptr || handler->is_partition_child || handler->locks_rows_on_read() ||
     handler->star_agg || handler->par_join || handler->par_agg || handler->pj_following()) {
    return (warp_pa_no(__LINE__), none);
  }
  {
    warp_trx *trx = warp_get_trx(handler->ht, thd);
    if(trx != nullptr && (trx->isolation_level == ISO_SERIALIZABLE || trx->lock_in_share_mode)) {
      return (warp_pa_no(__LINE__), none);
    }
  }
  if(warp_pa_rows_of(handler) < THDVAR(thd, parallel_join_min_rows)) {
    return (warp_pa_no(__LINE__), none);
  }

  auto plan = std::make_shared<warp_pa_plan>();
  plan->thd = thd;
  plan->table = table;
  plan->handler = handler;

  /* GROUP BY: columns of the table */
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
  for(; group != nullptr; group = group->next) {
    Item *item = warp_star_resolve(thd, *group->item, stream);
    if(item == nullptr || item->real_item()->type() != Item::FIELD_ITEM) {
      return (warp_pa_no(__LINE__), none);
    }
    Field *field = down_cast<Item_field *>(item->real_item())->field;
    if(field == nullptr || field->table != table) {
      return (warp_pa_no(__LINE__), none);
    }
    const int slot = warp_pa_add_col(plan.get(), field);
    if(slot < 0) {
      return none;
    }
    if(std::find(plan->key_cols.begin(), plan->key_cols.end(), slot) == plan->key_cols.end()) {
      plan->key_cols.push_back(slot);
    }
  }
  plan->group_cols = plan->key_cols.size();

  /* the aggregates */
  if(join->sum_funcs == nullptr || *join->sum_funcs == nullptr) {
    return (warp_pa_no(__LINE__), none);
  }
  std::vector<int> distinct_cols;
  for(Item_sum **it = join->sum_funcs; *it != nullptr; ++it) {
    Item_sum *sum = *it;
    warp_pa_agg agg;
    agg.item = sum;
    const Item_sum::Sumfunctype type = sum->sum_func();
    if(type == Item_sum::COUNT_DISTINCT_FUNC || type == Item_sum::SUM_DISTINCT_FUNC ||
       type == Item_sum::AVG_DISTINCT_FUNC) {
      /* not replaced: the groups are split by the columns that it reads */
      for(uint i = 0; i < sum->argument_count(); ++i) {
        Item *arg = warp_star_resolve(thd, sum->get_arg(i), stream);
        if(arg == nullptr || !warp_pa_fields_of(arg->real_item(), plan.get(), &distinct_cols)) {
          return (warp_pa_no(__LINE__), none);
        }
      }
      agg.replaced = false;
      plan->aggs.push_back(std::move(agg));
      continue;
    }
    if(sum->has_with_distinct() || sum->argument_count() != 1) {
      return (warp_pa_no(__LINE__), none);
    }
    Item *arg = warp_star_resolve(thd, sum->get_arg(0), stream);
    if(arg == nullptr) {
      return (warp_pa_no(__LINE__), none);
    }
    agg.replaced = true;
    switch(type) {
      case Item_sum::COUNT_FUNC: {
        /* COUNT(*) has a constant, COUNT(x) counts the values that are not NULL */
        if(arg->type() == Item::INT_ITEM || arg->const_item()) {
          agg.kind = warp_pa_agg::COUNT_ALL;
          break;
        }
        agg.kind = warp_pa_agg::COUNT_COL;
        if(arg->real_item()->type() == Item::FIELD_ITEM) {
          Field *field = down_cast<Item_field *>(arg->real_item())->field;
          if(field == nullptr || field->table != table) return (warp_pa_no(__LINE__), none);
          agg.str_col = warp_pa_add_col(plan.get(), field);
          if(agg.str_col < 0) return none;
        } else if(!warp_pa_program(arg, plan.get(), &agg.prog)) {
          return none;
        }
        break;
      }
      case Item_sum::SUM_FUNC:
      case Item_sum::AVG_FUNC: {
        agg.kind = type == Item_sum::SUM_FUNC ? warp_pa_agg::SUM : warp_pa_agg::AVG;
        if(!warp_pa_program(arg, plan.get(), &agg.prog)) return none;
        if(agg.prog.real) {
          if(sum->result_type() != REAL_RESULT && !(agg.kind == warp_pa_agg::AVG && sum->result_type() == REAL_RESULT)) {
            return (warp_pa_no(__LINE__), none);
          }
          agg.result_kind = PA_REAL;
        } else {
          if(sum->result_type() != DECIMAL_RESULT) return (warp_pa_no(__LINE__), none);
          agg.result_kind = PA_DEC;
          agg.result_scale = agg.prog.scale;
        }
        break;
      }
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC: {
        agg.kind = type == Item_sum::MIN_FUNC ? warp_pa_agg::MIN : warp_pa_agg::MAX;
        if(arg->real_item()->type() == Item::FIELD_ITEM &&
           down_cast<Item_field *>(arg->real_item())->field != nullptr &&
           down_cast<Item_field *>(arg->real_item())->field->table == table) {
          Field *field = down_cast<Item_field *>(arg->real_item())->field;
          const int slot = warp_pa_add_col(plan.get(), field);
          if(slot < 0) return none;
          if(plan->cols[slot].kind == PA_STR) {
            agg.str_col = slot;
            agg.result_kind = PA_STR;
            break;
          }
        }
        if(!warp_pa_program(arg, plan.get(), &agg.prog)) return none;
        if(agg.prog.real) {
          if(sum->result_type() != REAL_RESULT) return (warp_pa_no(__LINE__), none);
          agg.result_kind = PA_REAL;
        } else if(sum->result_type() == DECIMAL_RESULT) {
          agg.result_kind = PA_DEC;
          agg.result_scale = agg.prog.scale;
        } else if(sum->result_type() == INT_RESULT) {
          agg.result_kind = PA_INT;
        } else {
          return (warp_pa_no(__LINE__), none);
        }
        break;
      }
      default:
        return (warp_pa_no(__LINE__), none);
    }
    plan->aggs.push_back(std::move(agg));
  }

  if(distinct_cols.size() > 8) {
    return (warp_pa_no(__LINE__), none);
  }
  for(int slot : distinct_cols) {
    if(std::find(plan->key_cols.begin(), plan->key_cols.end(), slot) == plan->key_cols.end()) {
      plan->key_cols.push_back(slot);
    }
  }

  /* A COUNT(*) of the rows that pass a condition is counted by the scan that
     MySQL makes (and without a condition by a count of the rows that are
     visible, without reading them): the engine has nothing to add */
  if(plan->group_cols == 0 && plan->key_cols.empty()) {
    bool only_count = true;
    for(const auto &agg : plan->aggs) {
      if(agg.kind != warp_pa_agg::COUNT_ALL || !agg.replaced) only_count = false;
    }
    if(only_count) {
      return (warp_pa_no(__LINE__), none);
    }
  }

  /* The fields that MySQL reads from the rows of the engine are the columns of
     the GROUP BY and of the DISTINCT aggregates.  A column of the read set that
     is neither (a column that is not in the GROUP BY, with ONLY_FULL_GROUP_BY
     off) has no value in them; the columns that the replaced aggregates read
     are not read by MySQL any more. */
  {
    std::set<uint> allowed;
    /* the columns of the conditions that the engine evaluates */
    {
      const std::string &w = handler->push_where_clause;
      for(size_t i = 0; i + 1 < w.size(); ++i) {
        if((w[i] == 'c' || w[i] == 'n') && (i == 0 || !isalnum((unsigned char)w[i - 1])) &&
           isdigit((unsigned char)w[i + 1])) {
          uint f = 0;
          size_t j = i + 1;
          while(j < w.size() && isdigit((unsigned char)w[j])) {
            f = f * 10 + (w[j] - '0');
            ++j;
          }
          allowed.insert(f);
        }
      }
    }
    for(int c : plan->key_cols) allowed.insert(plan->cols[c].field);
    for(const auto &agg : plan->aggs) {
      if(!agg.replaced) continue;
      for(int c : agg.prog.cols) allowed.insert(plan->cols[c].field);
      if(agg.str_col >= 0) allowed.insert(plan->cols[agg.str_col].field);
    }
    for(Field **field = table->field; *field; ++field) {
      if(bitmap_is_set(table->read_set, (*field)->field_index()) &&
         allowed.count((*field)->field_index()) == 0) {
        return (warp_pa_no(__LINE__), none);
      }
    }
  }

  /* The rows are sorted between the engine and the aggregation: the constants
     that replace the arguments would have the values of the last row */
  if(sorted && stream != nullptr) {
    for(const auto &agg : plan->aggs) {
      if(agg.replaced) return (warp_pa_no(__LINE__), none);
    }
  }
  plan->adjacent = sorted;
  /* HAVING: the groups that do not pass are not returned (MySQL checks the
     condition again).  Only if every group of MySQL is one group of the
     engine, and a query without GROUP BY always has a row. */
  if(join->having_cond != nullptr && plan->group_cols > 0 && plan->key_cols.size() == plan->group_cols) {
    std::vector<Item *> conjuncts;
    std::function<void(Item *)> split = [&](Item *item) {
      if(item->type() == Item::COND_ITEM &&
         down_cast<Item_cond *>(item)->functype() == Item_func::COND_AND_FUNC) {
        List_iterator_fast<Item> list(*down_cast<Item_cond *>(item)->argument_list());
        for(Item *part = list++; part != nullptr; part = list++) split(part);
      } else {
        conjuncts.push_back(item);
      }
    };
    split(join->having_cond);
    for(Item *conjunct : conjuncts) {
      if(conjunct->type() != Item::FUNC_ITEM) continue;
      Item_func *func = down_cast<Item_func *>(conjunct);
      Item_func::Functype op = func->functype();
      if(func->argument_count() != 2 ||
         (op != Item_func::GT_FUNC && op != Item_func::GE_FUNC && op != Item_func::LT_FUNC &&
          op != Item_func::LE_FUNC && op != Item_func::EQ_FUNC && op != Item_func::NE_FUNC)) {
        continue;
      }
      Item *left = func->arguments()[0]->real_item();
      Item *right = func->arguments()[1]->real_item();
      Item *constant = right;
      Item *aggregate = left;
      if(left->const_item() && !right->const_item()) {
        std::swap(left, right);
        aggregate = left;
        constant = right;
        if(op == Item_func::GT_FUNC) op = Item_func::LT_FUNC;
        else if(op == Item_func::GE_FUNC) op = Item_func::LE_FUNC;
        else if(op == Item_func::LT_FUNC) op = Item_func::GT_FUNC;
        else if(op == Item_func::LE_FUNC) op = Item_func::GE_FUNC;
      }
      size_t index = plan->aggs.size();
      for(size_t m = 0; m < plan->aggs.size(); ++m) {
        if(plan->aggs[m].item == aggregate && plan->aggs[m].replaced) index = m;
      }
      if(index == plan->aggs.size() || !constant->const_item() ||
         (constant->type() != Item::INT_ITEM && constant->type() != Item::DECIMAL_ITEM &&
          constant->type() != Item::REAL_ITEM)) {
        continue;
      }
      const warp_pa_agg &agg = plan->aggs[index];
      if(agg.kind == warp_pa_agg::AVG) continue;
      warp_pa_having h;
      h.agg = index;
      h.op = op;
      if(agg.kind == warp_pa_agg::COUNT_ALL || agg.kind == warp_pa_agg::COUNT_COL || agg.result_kind == PA_INT) {
        if(constant->type() != Item::INT_ITEM) continue;
        h.ivalue = constant->val_int();
        h.iscale = 0;
      } else if(agg.result_kind == PA_DEC) {
        if(constant->type() == Item::REAL_ITEM) continue;
        my_decimal buffer;
        my_decimal *dec = constant->val_decimal(&buffer);
        if(dec == nullptr || constant->decimals > 18) continue;
        String digits;
        my_decimal2string(E_DEC_FATAL_ERROR, dec, 0, constant->decimals, &digits);
        warp_i128 v = 0;
        bool negative = false;
        for(size_t i = 0; i < digits.length(); ++i) {
          const char ch = digits.ptr()[i];
          if(ch == '-') negative = true;
          else if(ch >= '0' && ch <= '9') v = v * 10 + (ch - '0');
        }
        h.ivalue = negative ? -v : v;
        h.iscale = constant->decimals;
      } else if(agg.result_kind == PA_REAL) {
        h.dvalue = constant->val_real();
      } else {
        continue;
      }
      plan->having.push_back(h);
    }
  }
  return plan;
}


/* The plan for the scan below a materialization that removes duplicates (the
   temporary table of a semijoin, of a DISTINCT): the rows of the scan that
   have the columns `fields` are the distinct combinations of their values. */
static std::shared_ptr<warp_pa_plan> warp_pa_make_distinct_plan(THD *thd, AccessPath *scan_path,
                                                                const std::vector<Field *> &fields) {
  std::shared_ptr<warp_pa_plan> none;
  if(scan_path->type != AccessPath::TABLE_SCAN || fields.empty()) {
    return (warp_pa_no(__LINE__), none);
  }
  TABLE *table = scan_path->table_scan().table;
  ha_warp *handler = table == nullptr ? nullptr : dynamic_cast<ha_warp *>(table->file);
  if(handler == nullptr || handler->is_partition_child || handler->locks_rows_on_read() ||
     handler->star_agg || handler->par_join || handler->par_agg || handler->pj_following()) {
    return (warp_pa_no(__LINE__), none);
  }
  {
    warp_trx *trx = warp_get_trx(handler->ht, thd);
    if(trx != nullptr && (trx->isolation_level == ISO_SERIALIZABLE || trx->lock_in_share_mode)) {
      return (warp_pa_no(__LINE__), none);
    }
  }
  if(warp_pa_rows_of(handler) < THDVAR(thd, parallel_join_min_rows)) {
    return (warp_pa_no(__LINE__), none);
  }
  auto plan = std::make_shared<warp_pa_plan>();
  plan->thd = thd;
  plan->table = table;
  plan->handler = handler;
  for(Field *field : fields) {
    if(field->table != table) {
      return (warp_pa_no(__LINE__), none);
    }
    const int slot = warp_pa_add_col(plan.get(), field);
    if(slot < 0) {
      return none;
    }
    if(std::find(plan->key_cols.begin(), plan->key_cols.end(), slot) == plan->key_cols.end()) {
      plan->key_cols.push_back(slot);
    }
  }
  plan->group_cols = plan->key_cols.size();
  /* MySQL reads the columns that are returned (and the ones of the conditions
     that the engine evaluates) */
  {
    std::set<uint> allowed;
    const std::string &w = handler->push_where_clause;
    for(size_t i = 0; i + 1 < w.size(); ++i) {
      if((w[i] == 'c' || w[i] == 'n') && (i == 0 || !isalnum((unsigned char)w[i - 1])) &&
         isdigit((unsigned char)w[i + 1])) {
        uint f = 0;
        size_t j = i + 1;
        while(j < w.size() && isdigit((unsigned char)w[j])) {
          f = f * 10 + (w[j] - '0');
          ++j;
        }
        allowed.insert(f);
      }
    }
    for(int c : plan->key_cols) allowed.insert(plan->cols[c].field);
    for(Field **field = table->field; *field; ++field) {
      if(bitmap_is_set(table->read_set, (*field)->field_index()) && allowed.count((*field)->field_index()) == 0) {
        return (warp_pa_no(__LINE__), none);
      }
    }
  }
  return plan;
}

/* The scan below a materialization with deduplication returns the distinct
   rows (MySQL removes the duplicates that are left). */
static bool warp_pa_replace_distinct(THD *thd, AccessPath *materialize) {
  const MaterializePathParameters *param = materialize->materialize().param;
  if(param == nullptr || param->rematerialize || !MaterializeIsDoingDeduplication(param->table) ||
     param->m_operands.size() != 1 || param->cte != nullptr) {
    return false;
  }
  const MaterializePathParameters::Operand &operand = param->m_operands[0];
  if(operand.temp_table_param == nullptr || operand.temp_table_param->items_to_copy == nullptr ||
     operand.subquery_path == nullptr) {
    return warp_pa_no(__LINE__);
  }
  std::vector<Field *> fields;
  for(const Func_ptr &entry : *operand.temp_table_param->items_to_copy) {
    Item *source = entry.func()->real_item();
    if(source->type() != Item::FIELD_ITEM || down_cast<Item_field *>(source)->field == nullptr) {
      return warp_pa_no(__LINE__);
    }
    fields.push_back(down_cast<Item_field *>(source)->field);
  }
  AccessPath **slot = const_cast<AccessPath **>(&operand.subquery_path);
  /* a condition that MySQL evaluates (b.k IS NOT NULL) stays above the scan if
     it reads the returned columns only: a distinct row passes it if its
     duplicates do */
  while((*slot)->type == AccessPath::FILTER) {
    bool only_key_columns = true;
    WalkItem((*slot)->filter().condition, enum_walk::PREFIX, [&](Item *item) {
      if(item->type() == Item::FIELD_ITEM) {
        Field *field = down_cast<Item_field *>(item)->field;
        if(std::find(fields.begin(), fields.end(), field) == fields.end()) only_key_columns = false;
      } else if(item->has_subquery() || item->has_stored_program() || item->is_non_deterministic()) {
        only_key_columns = false;
      }
      return false;
    });
    if(!only_key_columns) {
      return warp_pa_no(__LINE__);
    }
    slot = &(*slot)->filter().child;
  }
  std::shared_ptr<warp_pa_plan> plan = warp_pa_make_distinct_plan(thd, *slot, fields);
  if(!plan) {
    return false;
  }
  for(int slot_index : plan->key_cols) {
    bitmap_set_bit(plan->table->read_set, plan->cols[slot_index].field);
  }
  AccessPath *scan = NewTableScanAccessPath(thd, plan->table, false);
  scan->set_num_output_rows((*slot)->num_output_rows());
  scan->set_cost((*slot)->cost());
  *slot = scan;
  plan->handler->par_agg = plan;
  return true;
}

/* ---------------------------------------------------------------------- */
/* execution                                                              */
/* ---------------------------------------------------------------------- */

/* the digits of a decimal as an integer with the scale of the column */
static inline bool warp_pa_parse_decimal(const std::string &s, int scale, int64_t *out) {
  size_t i = 0;
  bool negative = false;
  if(i < s.size() && (s[i] == '-' || s[i] == '+')) {
    negative = s[i] == '-';
    ++i;
  }
  int64_t v = 0;
  int digits_after = -1;
  for(; i < s.size(); ++i) {
    const char c = s[i];
    if(c == '.') {
      if(digits_after >= 0) return false;
      digits_after = 0;
    } else if(c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      if(digits_after >= 0) ++digits_after;
    } else {
      return false;
    }
  }
  if(digits_after < 0) digits_after = 0;
  if(digits_after > scale) return false;
  for(; digits_after < scale; ++digits_after) v *= 10;
  *out = negative ? -v : v;
  return true;
}

/* the key that two strings have in common if the collation says they are
   equal */
static inline void warp_pa_collation_key(const CHARSET_INFO *cs, const std::string &s, std::string *key) {
  if(cs->state & MY_CS_BINSORT) {
    /* a binary collation: PAD SPACE ignores the spaces at the end */
    size_t n = s.size();
    if(cs->pad_attribute == PAD_SPACE) {
      while(n > 0 && s[n - 1] == ' ') --n;
    }
    key->assign(s.data(), n);
    return;
  }
  const size_t max_length = cs->coll->strnxfrmlen(cs, s.size());
  key->resize(max_length);
  const size_t length = cs->coll->strnxfrm(cs, (uchar *)&(*key)[0], max_length, (uint)max_length,
                                           (const uchar *)s.data(), s.size(), 0);
  key->resize(length);
}

/* the values of one column of one partition */
struct warp_pa_loaded {
  warp_star_col ints;
  ibis::array_t<double> doubles;
  ibis::array_t<float> floats;
  bool is_float = false;
  std::vector<int64_t> dec;                           // by the position of the row among the rows that pass the conditions
  std::unique_ptr<std::vector<std::string>> strs;     // the same
  warp_star_col nulls;
  bool has_nulls = false;
};


/* The groups of a worker (or of the merge): the keys are in one buffer, the
   groups in blocks, and the table is open addressing, so that a group costs no
   allocation for its key and its place in the table. */
struct warp_pa_table {
  struct Entry {
    uint64_t hash;
    size_t offset;
    uint32_t length;
  };
  std::string keys;                      // the bytes of the keys
  std::vector<Entry> entries;            // by the index of the group
  std::deque<warp_pa_group> groups;      // the groups do not move when more are added
  std::vector<uint32_t> slots;           // index of the group + 1, 0 = free
  size_t mask = 0;

  size_t size() const { return entries.size(); }
  std::string_view key(size_t i) const { return std::string_view(keys.data() + entries[i].offset, entries[i].length); }
  static uint64_t hash_of(std::string_view k) { return std::_Hash_bytes(k.data(), k.size(), 0xc70f6907UL); }

  void grow() {
    const size_t n = slots.empty() ? 1024 : slots.size() * 2;
    slots.assign(n, 0);
    mask = n - 1;
    for(size_t i = 0; i < entries.size(); ++i) {
      size_t at = entries[i].hash & mask;
      while(slots[at] != 0) at = (at + 1) & mask;
      slots[at] = (uint32_t)(i + 1);
    }
  }
  /* the group of the key; *added tells whether it is a new one (then it is empty) */
  warp_pa_group *find_or_add(std::string_view k, uint64_t hash, bool *added) {
    if(slots.empty() || (entries.size() + 1) * 2 > slots.size()) grow();
    size_t at = hash & mask;
    while(slots[at] != 0) {
      const size_t i = slots[at] - 1;
      if(entries[i].hash == hash && entries[i].length == k.size() &&
         memcmp(keys.data() + entries[i].offset, k.data(), k.size()) == 0) {
        *added = false;
        return &groups[i];
      }
      at = (at + 1) & mask;
    }
    entries.push_back({hash, keys.size(), (uint32_t)k.size()});
    keys.append(k.data(), k.size());
    groups.emplace_back();
    slots[at] = (uint32_t)entries.size();
    *added = true;
    return &groups.back();
  }
  warp_pa_group *find_or_add(std::string_view k, bool *added) { return find_or_add(k, hash_of(k), added); }
};

struct warp_pa_worker {
  warp_pa_plan *plan = nullptr;
  warp_pa_table groups;
};

static inline void warp_pa_init_group(const warp_pa_plan &plan, warp_pa_group *g) {
  g->states.resize(plan.aggs.size());
}

/* Evaluates a program for a row.  Returns false if a value is NULL.  A
   BIGINT overflow sets *overflow. */
static inline bool warp_pa_eval_int(const warp_pa_prog &prog, const std::vector<warp_pa_col> &cols,
                                    const std::vector<warp_pa_loaded> &loaded, size_t i, size_t row,
                                    warp_i128 *result, bool *overflow) {
  warp_i128 stack[16];
  int top = 0;
  for(int c : prog.cols) {
    if(loaded[c].has_nulls && loaded[c].nulls.get(row) != 0) return false;
  }
  bool all_int = true;
  for(const warp_pa_op &op : prog.ops) {
    switch(op.kind) {
      case warp_pa_op::COLUMN:
        if(cols[op.slot].kind == PA_DEC) {
          stack[top++] = loaded[op.slot].dec[i];
        } else {
          stack[top++] = loaded[op.slot].ints.get(row);
        }
        break;
      case warp_pa_op::CONSTANT:
        stack[top++] = op.value;
        break;
      default: {
        warp_i128 b = stack[--top];
        warp_i128 a = stack[--top];
        warp_i128 r;
        if(op.kind == warp_pa_op::MUL) {
          r = a * b;
        } else {
          if(op.shift_a) a *= warp_pa_pow10(op.shift_a);
          if(op.shift_b) b *= warp_pa_pow10(op.shift_b);
          r = op.kind == warp_pa_op::ADD ? a + b : a - b;
        }
        stack[top++] = r;
      }
    }
  }
  (void)all_int;
  *result = stack[0];
  /* MySQL computes an integer expression in BIGINT */
  if(prog.scale == 0 && (*result > INT64_MAX || *result < INT64_MIN)) {
    *overflow = true;
  }
  return true;
}

static inline bool warp_pa_eval_real(const warp_pa_prog &prog, const std::vector<warp_pa_col> &cols,
                                     const std::vector<warp_pa_loaded> &loaded, size_t i, size_t row,
                                     double *result) {
  double stack[16];
  int top = 0;
  for(int c : prog.cols) {
    if(loaded[c].has_nulls && loaded[c].nulls.get(row) != 0) return false;
  }
  for(const warp_pa_op &op : prog.ops) {
    switch(op.kind) {
      case warp_pa_op::COLUMN: {
        const warp_pa_col &col = cols[op.slot];
        if(col.kind == PA_REAL) {
          stack[top++] = loaded[op.slot].is_float ? (double)loaded[op.slot].floats[row] : loaded[op.slot].doubles[row];
        } else if(col.kind == PA_DEC) {
          stack[top++] = (double)loaded[op.slot].dec[i] / (double)warp_pa_pow10(col.scale);
        } else {
          stack[top++] = (double)loaded[op.slot].ints.get(row);
        }
        break;
      }
      case warp_pa_op::CONSTANT:
        stack[top++] = op.dvalue;
        break;
      default: {
        const double b = stack[--top];
        const double a = stack[--top];
        stack[top++] = op.kind == warp_pa_op::ADD ? a + b : (op.kind == warp_pa_op::SUB ? a - b : a * b);
      }
    }
  }
  *result = stack[0];
  return true;
}

/* adds the rows [lo, hi) of the rows that pass the conditions of a partition */
static void warp_pa_slice(const warp_pa_plan &plan, const std::vector<warp_pa_loaded> &loaded,
                          const std::vector<uint32_t> &hit_rows, const std::vector<uint8_t> &invisible,
                          size_t lo, size_t hi, warp_pa_worker *worker) {
  const size_t nkeys = plan.key_cols.size();
  std::string key;
  std::string collation_key;
  std::vector<warp_pa_cell> cells(nkeys);
  for(size_t i = lo; i < hi; ++i) {
    const size_t row = hit_rows[i];
    if(!invisible.empty() && invisible[row]) {
      continue;
    }
    if(plan.semi_active) {
      const warp_pa_loaded &kl = loaded[plan.semi_slot];
      if((kl.has_nulls && kl.nulls.get(row) != 0) || !plan.semi_has(kl.ints.get(row))) {
        continue;
      }
    }
    /* the group */
    key.clear();
    for(size_t k = 0; k < nkeys; ++k) {
      const int c = plan.key_cols[k];
      const warp_pa_col &col = plan.cols[c];
      const warp_pa_loaded &ld = loaded[c];
      warp_pa_cell &cell = cells[k];
      cell.null = ld.has_nulls && ld.nulls.get(row) != 0;
      if(cell.null) {
        key.push_back('\1');
        continue;
      }
      key.push_back('\0');
      switch(col.kind) {
        case PA_INT:
          cell.i = ld.ints.get(row);
          key.append((const char *)&cell.i, sizeof(cell.i));
          break;
        case PA_DEC:
          cell.i = ld.dec[i];
          key.append((const char *)&cell.i, sizeof(cell.i));
          break;
        case PA_REAL:
          cell.d = ld.is_float ? (double)ld.floats[row] : ld.doubles[row];
          if(cell.d == 0) cell.d = 0;   // -0 and 0 are one value
          key.append((const char *)&cell.d, sizeof(cell.d));
          break;
        default: {
          const std::string &s = (*ld.strs)[i];
          warp_pa_collation_key(col.cs, s, &collation_key);
          const uint32_t length = (uint32_t)collation_key.size();
          key.append((const char *)&length, sizeof(length));
          key.append(collation_key);
          cell.s = s;
        }
      }
    }
    bool added;
    warp_pa_group &group = *worker->groups.find_or_add(key, &added);
    if(added) {
      warp_pa_init_group(plan, &group);
      group.cells = cells;
    }
    ++group.rows;
    /* the aggregates */
    for(size_t m = 0; m < plan.aggs.size(); ++m) {
      const warp_pa_agg &agg = plan.aggs[m];
      if(!agg.replaced) {
        continue;
      }
      warp_pa_state &st = group.states[m];
      switch(agg.kind) {
        case warp_pa_agg::COUNT_ALL:
          break;
        case warp_pa_agg::COUNT_COL: {
          if(agg.str_col >= 0) {
            const warp_pa_loaded &ld = loaded[agg.str_col];
            if(!(ld.has_nulls && ld.nulls.get(row) != 0)) ++st.count;
          } else if(agg.prog.real) {
            double d;
            if(warp_pa_eval_real(agg.prog, plan.cols, loaded, i, row, &d)) ++st.count;
          } else {
            warp_i128 v;
            bool overflow = false;
            if(warp_pa_eval_int(agg.prog, plan.cols, loaded, i, row, &v, &overflow)) ++st.count;
          }
          break;
        }
        case warp_pa_agg::SUM:
        case warp_pa_agg::AVG: {
          if(agg.prog.real) {
            double d;
            if(warp_pa_eval_real(agg.prog, plan.cols, loaded, i, row, &d)) {
              st.dsum += d;
              ++st.count;
            }
          } else {
            warp_i128 v;
            bool overflow = false;
            if(warp_pa_eval_int(agg.prog, plan.cols, loaded, i, row, &v, &overflow)) {
              if(overflow) {
                const_cast<warp_pa_plan &>(plan).overflow = true;
                return;
              }
              st.isum += v;
              ++st.count;
            }
          }
          break;
        }
        case warp_pa_agg::MIN:
        case warp_pa_agg::MAX: {
          const bool is_min = agg.kind == warp_pa_agg::MIN;
          if(agg.result_kind == PA_STR) {
            const warp_pa_loaded &ld = loaded[agg.str_col];
            if(ld.has_nulls && ld.nulls.get(row) != 0) break;
            const std::string &s = (*ld.strs)[i];
            warp_pa_collation_key(plan.cols[agg.str_col].cs, s, &collation_key);
            ++st.count;
            if(!st.have || (is_min ? collation_key < st.skey : collation_key > st.skey)) {
              st.have = true;
              st.skey = collation_key;
              st.sval = s;
            }
          } else if(agg.result_kind == PA_REAL) {
            double d;
            if(!warp_pa_eval_real(agg.prog, plan.cols, loaded, i, row, &d)) break;
            ++st.count;
            if(!st.have || (is_min ? d < st.dval : d > st.dval)) {
              st.have = true;
              st.dval = d;
            }
          } else {
            warp_i128 v;
            bool overflow = false;
            if(!warp_pa_eval_int(agg.prog, plan.cols, loaded, i, row, &v, &overflow)) break;
            ++st.count;
            if(!st.have || (is_min ? v < st.ival : v > st.ival)) {
              st.have = true;
              st.ival = v;
            }
          }
          break;
        }
      }
    }
  }
}

/* the rows of one partition */
static bool warp_pa_partition(warp_pa_plan &plan, ibis::part *part, const warp_star_snapshot &snap,
                              std::vector<warp_pa_worker> *results, std::string *error) {
  const size_t nrows = part->nRows();
  const char *dir = part->currentDataDir();

  /* the indexes of the columns of the condition are brought up to date */
  {
    std::vector<uint> stale;
    for(uint f : plan.where_fields) {
      const std::string data = std::string(dir) + "/c" + std::to_string(f);
      struct stat dst, ist;
      if(stat((data + ".idx").c_str(), &ist) == 0 &&
         (stat(data.c_str(), &dst) != 0 || dst.st_mtime > ist.st_mtime)) {
        stale.push_back(f);
      }
    }
    if(!stale.empty()) {
      warp_maintain_partition_indexes(dir, stale);
    }
  }

  ibis::bitvector everything;
  std::unique_ptr<ibis::query> query;
  const ibis::bitvector *hits = nullptr;
  if(plan.where.empty() || plan.where == "1=1") {
    everything.set(1, nrows);
    hits = &everything;
  } else {
    query.reset(new ibis::query((const char *)0, part, (const char *)0));
    query->addConditions(plan.where.c_str());
    if(query->evaluate() < 0) {
      *error = std::string("could not evaluate the condition on ") + dir;
      return false;
    }
    hits = query->getHitVector();
    if(hits == nullptr || hits->cnt() == 0) {
      return true;
    }
  }
  std::vector<uint8_t> invisible;
  if(!warp_star_invisible(part, snap, &invisible)) {
    *error = std::string("could not read the transactions of ") + dir;
    return false;
  }
  std::vector<uint32_t> hit_rows;
  hit_rows.reserve(hits->cnt());
  for(ibis::bitvector::indexSet ix = hits->firstIndexSet(); ix.nIndices() > 0; ++ix) {
    const ibis::bitvector::word_t *indices = ix.indices();
    if(ix.isRange()) {
      for(ibis::bitvector::word_t r = indices[0]; r < indices[1]; ++r) hit_rows.push_back(r);
    } else {
      for(ibis::bitvector::word_t j = 0; j < ix.nIndices(); ++j) hit_rows.push_back(indices[j]);
    }
  }

  /* the columns */
  std::vector<warp_pa_loaded> loaded(plan.cols.size());
  for(size_t c = 0; c < plan.cols.size(); ++c) {
    const warp_pa_col &col = plan.cols[c];
    warp_pa_loaded &ld = loaded[c];
    const std::string name = "c" + std::to_string(col.field);
    const ibis::column *column = part->getColumn(name.c_str());
    if(column == nullptr) {
      *error = std::string("could not read column ") + name + " of " + dir;
      return false;
    }
    switch(col.kind) {
      case PA_INT:
        if(!ld.ints.load(column, nrows)) {
          *error = std::string("could not read column ") + name + " of " + dir;
          return false;
        }
        break;
      case PA_REAL:
        if(column->type() == ibis::FLOAT) {
          ld.is_float = true;
          if(column->getValuesArray(&ld.floats) != 0 || ld.floats.size() != nrows) {
            *error = std::string("could not read column ") + name + " of " + dir;
            return false;
          }
        } else if(column->getValuesArray(&ld.doubles) != 0 || ld.doubles.size() != nrows) {
          *error = std::string("could not read column ") + name + " of " + dir;
          return false;
        }
        break;
      default: {
        ld.strs.reset(column->selectStrings(*hits));
        if(!ld.strs || ld.strs->size() != hit_rows.size()) {
          *error = std::string("could not read column ") + name + " of " + dir;
          return false;
        }
        if(col.kind == PA_DEC) {
          ld.dec.resize(ld.strs->size());
          for(size_t i = 0; i < ld.strs->size(); ++i) {
            if(!warp_pa_parse_decimal((*ld.strs)[i], col.scale, &ld.dec[i])) {
              /* a NULL value is stored as an empty string: its marker says so */
              ld.dec[i] = 0;
            }
          }
          ld.strs.reset();
        }
      }
    }
    if(col.nullable) {
      const std::string nname = "n" + std::to_string(col.field);
      if(!ld.nulls.load(part->getColumn(nname.c_str()), nrows)) {
        *error = std::string("could not read column ") + nname + " of " + dir;
        return false;
      }
      ld.has_nulls = true;
    }
  }

  /* the rows are processed in slices, at the same time if the thread budget of
     the statement has threads to spare */
  const size_t min_slice = std::max<size_t>(THDVAR(plan.thd, parallel_min_rows), 1);
  size_t nslices = 1;
  if(hit_rows.size() >= 2 * min_slice) {
    nslices = std::min<size_t>(hit_rows.size() / min_slice, 64);
  }
  ibis::util::ThreadLease lease(nslices > 1 ? (unsigned)(nslices - 1) : 0);
  nslices = lease.granted() + 1;
  std::vector<warp_pa_worker> workers(nslices);
  std::vector<std::exception_ptr> slice_error(nslices);
  auto run_slice = [&](size_t k) {
    try {
      workers[k].plan = &plan;
      warp_pa_slice(plan, loaded, hit_rows, invisible, hit_rows.size() * k / nslices,
                    hit_rows.size() * (k + 1) / nslices, &workers[k]);
    } catch(...) {
      slice_error[k] = std::current_exception();
    }
  };
  std::vector<std::thread> threads;
  size_t started = 1;
  for(; started < nslices; ++started) {
    try {
      const size_t k = started;
      const std::shared_ptr<ibis::util::ThreadBudget> budget = lease.budget();
      threads.emplace_back([&run_slice, budget, k]() {
        ibis::util::ThreadBudgetScope scope(budget);
        run_slice(k);
      });
    } catch(...) {
      break;
    }
  }
  run_slice(0);
  for(size_t k = started; k < nslices; ++k) run_slice(k);
  for(auto &t : threads) t.join();
  for(size_t k = 0; k < nslices; ++k) {
    if(slice_error[k]) std::rethrow_exception(slice_error[k]);
  }
  for(auto &w : workers) {
    results->push_back(std::move(w));
  }
  return true;
}

/* adds the group of another worker to the group */
static void warp_pa_merge_group(const warp_pa_plan &plan, warp_pa_group *dst, warp_pa_group &src) {
  dst->rows += src.rows;
  for(size_t m = 0; m < plan.aggs.size(); ++m) {
    const warp_pa_agg &agg = plan.aggs[m];
    warp_pa_state &d = dst->states[m];
    warp_pa_state &s = src.states[m];
    d.isum += s.isum;
    d.dsum += s.dsum;
    d.count += s.count;
    if(agg.kind == warp_pa_agg::MIN || agg.kind == warp_pa_agg::MAX) {
      if(!s.have) continue;
      const bool is_min = agg.kind == warp_pa_agg::MIN;
      bool better;
      if(!d.have) better = true;
      else if(agg.result_kind == PA_STR) better = is_min ? s.skey < d.skey : s.skey > d.skey;
      else if(agg.result_kind == PA_REAL) better = is_min ? s.dval < d.dval : s.dval > d.dval;
      else better = is_min ? s.ival < d.ival : s.ival > d.ival;
      if(better) {
        d.have = true;
        d.ival = s.ival;
        d.dval = s.dval;
        d.sval = std::move(s.sval);
        d.skey = std::move(s.skey);
      }
    }
  }
}

/* The groups of all the partitions.  A partition adds its groups when it is done
   (and frees its own table), so that the memory is the groups that exist, not
   the groups of every partition: a column with millions of values has that
   many groups in every partition. */
struct warp_pa_shared {
  static constexpr size_t SHARDS = 64;
  std::mutex mutex[SHARDS];
  warp_pa_table table[SHARDS];
};

static void warp_pa_merge_into(const warp_pa_plan &plan, warp_pa_shared *shared, std::vector<warp_pa_worker> *local) {
  for(auto &w : *local) {
    /* the groups of one shard are added with the lock taken once */
    std::vector<std::vector<uint32_t>> by_shard(warp_pa_shared::SHARDS);
    for(size_t i = 0; i < w.groups.size(); ++i) {
      by_shard[(w.groups.entries[i].hash >> 58) & (warp_pa_shared::SHARDS - 1)].push_back((uint32_t)i);
    }
    for(size_t s = 0; s < warp_pa_shared::SHARDS; ++s) {
      if(by_shard[s].empty()) continue;
      std::lock_guard<std::mutex> guard(shared->mutex[s]);
      for(uint32_t i : by_shard[s]) {
        bool added;
        warp_pa_group *dst = shared->table[s].find_or_add(w.groups.key(i), w.groups.entries[i].hash, &added);
        if(added) {
          *dst = std::move(w.groups.groups[i]);
        } else {
          warp_pa_merge_group(plan, dst, w.groups.groups[i]);
        }
      }
    }
    w.groups = warp_pa_table();
  }
  local->clear();
}

static bool warp_pa_having_passes(const warp_pa_plan &plan, const warp_pa_group &group);

/* the groups of the shards are the result.  The groups that HAVING leaves out
   are not kept (an aggregation of millions of groups with a selective HAVING
   would otherwise move and free all of them one by one), and the shards are
   freed by several threads. */
static void warp_pa_collect(warp_pa_plan &plan, warp_pa_shared &shared) {
  const bool filter = !plan.having.empty();
  size_t workers = std::max<size_t>(1, THDVAR(plan.thd, max_degree_of_parallelism));
  workers = std::min<size_t>(workers, warp_pa_shared::SHARDS);
  auto parallel_shards = [&](const std::function<void(size_t)> &work) {
    std::atomic<size_t> next(0);
    std::vector<std::thread> threads;
    for(size_t w = 0; w < workers; ++w) {
      threads.emplace_back([&]() {
        for(size_t t; (t = next.fetch_add(1)) < warp_pa_shared::SHARDS;) work(t);
      });
    }
    for(auto &thread : threads) thread.join();
  };
  if(plan.adjacent) {
    /* the key of a group starts with the key of the GROUP BY columns, which
       is not followed by anything that is not part of the key of the
       columns: groups with the same prefix are next to each other in the
       order of the keys */
    std::vector<std::pair<uint32_t, uint32_t>> order;
    for(size_t t = 0; t < warp_pa_shared::SHARDS; ++t) {
      for(size_t i = 0; i < shared.table[t].size(); ++i) {
        if(!filter || warp_pa_having_passes(plan, shared.table[t].groups[i])) order.push_back({(uint32_t)t, (uint32_t)i});
      }
    }
    std::sort(order.begin(), order.end(), [&](const auto &a, const auto &b) {
      return shared.table[a.first].key(a.second) < shared.table[b.first].key(b.second);
    });
    plan.groups.reserve(order.size());
    for(const auto &entry : order) {
      plan.groups.push_back(std::move(shared.table[entry.first].groups[entry.second]));
    }
    parallel_shards([&](size_t t) { shared.table[t] = warp_pa_table(); });
  } else {
    std::vector<std::vector<warp_pa_group>> kept(warp_pa_shared::SHARDS);
    parallel_shards([&](size_t t) {
      for(auto &group : shared.table[t].groups) {
        if(!filter || warp_pa_having_passes(plan, group)) kept[t].push_back(std::move(group));
      }
      shared.table[t] = warp_pa_table();
    });
    size_t total = 0;
    for(const auto &k : kept) total += k.size();
    plan.groups.reserve(total);
    for(auto &k : kept) {
      for(auto &group : k) plan.groups.push_back(std::move(group));
    }
  }
}

/* ---------------------------------------------------------------------- */
/* the aggregation of the rows of the join of the engine                  */
/* ---------------------------------------------------------------------- */

struct warp_pja_agg {
  warp_pa_agg::Kind kind = warp_pa_agg::COUNT_ALL;
  warp_pj_ref ref;
};

/* The rows of the result of the join are not made, they are added to the groups
   of the aggregation (the groups of a slice of a partition are merged into the
   shared ones when the slice is done). */
/* The groups of an aggregation of the rows of a join of the engine: the shared
   groups that the slices of the partitions are merged into */
struct warp_pa_sink_base : warp_pj_sink {
  warp_pa_plan *pa = nullptr;
  std::unique_ptr<warp_pa_shared> shared;
  std::atomic<bool> failed{false};
};

struct warp_pja_sink : warp_pa_sink_base {
  std::vector<warp_pj_ref> group;       // the columns of the groups, in the order of key_cols
  std::vector<warp_pja_agg> aggs;       // in the order of the aggregates of the plan

  struct Local {
    std::vector<warp_pa_worker> w;
    std::string key;
    std::vector<warp_pa_cell> cells;
  };

  void *begin() override {
    Local *l = new Local;
    l->w.resize(1);
    l->cells.resize(group.size());
    return l;
  }

  void add(void *local, const warp_pj_prober &p) override {
    Local &l = *static_cast<Local *>(local);
    l.key.clear();
    for(size_t k = 0; k < group.size(); ++k) {
      int64_t v;
      warp_pa_cell &cell = l.cells[k];
      if(p.value(group[k], &v)) {
        cell.null = false;
        cell.i = v;
        l.key.push_back('\0');
        l.key.append((const char *)&v, sizeof(v));
      } else {
        cell.null = true;
        cell.i = 0;
        l.key.push_back('\1');
      }
    }
    bool added;
    warp_pa_group *g = l.w[0].groups.find_or_add(l.key, &added);
    if(added) {
      g->states.resize(aggs.size());
      g->cells = l.cells;
    }
    ++g->rows;
    for(size_t m = 0; m < aggs.size(); ++m) {
      warp_pa_state &st = g->states[m];
      int64_t v;
      switch(aggs[m].kind) {
        case warp_pa_agg::COUNT_ALL:
          break;
        case warp_pa_agg::COUNT_COL:
          if(p.value(aggs[m].ref, &v)) ++st.count;
          break;
        case warp_pa_agg::SUM:
          if(p.value(aggs[m].ref, &v)) {
            st.isum += v;
            ++st.count;
          }
          break;
        case warp_pa_agg::MIN:
        case warp_pa_agg::MAX:
          if(p.value(aggs[m].ref, &v)) {
            ++st.count;
            if(!st.have || (aggs[m].kind == warp_pa_agg::MIN ? v < st.ival : v > st.ival)) {
              st.have = true;
              st.ival = v;
            }
          }
          break;
        default:
          break;
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

static bool warp_pja_execute(warp_pa_plan &plan, std::string *error) {
  warp_pa_sink_base *sink = static_cast<warp_pa_sink_base *>(plan.pj->sink.get());
  sink->shared.reset(new warp_pa_shared);
  sink->failed = false;
  plan.groups.clear();
  plan.pos = 0;
  plan.overflow = false;
  if(!warp_pj_execute(*plan.pj, error)) {
    return false;
  }
  if(sink->failed) {
    *error = "out of memory";
    return false;
  }
  warp_pa_collect(plan, *sink->shared);
  sink->shared.reset();
  return true;
}

static bool warp_pa_execute(warp_pa_plan &plan, std::string *error) {
  if(plan.pj) {
    return warp_pja_execute(plan, error);
  }
  THD *thd = plan.thd;
  plan.groups.clear();
  plan.pos = 0;
  plan.overflow = false;
  warp_star_snapshot snap;
  warp_trx *trx = warp_get_trx(plan.handler->ht, thd);
  if(trx == nullptr) {
    *error = "no transaction";
    return false;
  }
  snap.trx_id = trx->trx_id;
  snap.repeatable_read = (trx->isolation_level == ISO_REPEATABLE_READ ||
                          trx->isolation_level == ISO_SERIALIZABLE);
  snap.check_rowids = warp_state->has_history_locks() || warp_state->delete_bitmap->may_have_bits();

  warp_table_read_lock lock(plan.handler->get_warp_share()->data_dir_name);
  ibis::partList parts;
  ibis::util::gatherParts(parts, plan.handler->get_warp_share()->data_dir_name, true);
  std::vector<ibis::part *> work;
  for(ibis::part *part : parts) {
    if(part != nullptr && part->nRows() > 0 &&
       std::string(part->currentDataDir()) != std::string(plan.handler->get_warp_share()->data_dir_name)) {
      work.push_back(part);
    }
  }
  size_t workers = THDVAR(thd, max_degree_of_parallelism);
  if(workers < 1) workers = 1;
  if(!work.empty()) {
    const uint64_t per_worker = std::max<uint64_t>(work[0]->nRows(), 1) * 12 * (plan.cols.size() + 2);
    workers = std::min<uint64_t>(workers, std::max<uint64_t>(1, (ibis::fileManager::currentCacheSize() / 2) / per_worker));
  }
  workers = std::min(workers, std::max<size_t>(work.size(), 1));

  warp_pa_shared shared;
  std::atomic<size_t> next(0);
  std::atomic<bool> failed(false);
  std::mutex error_mutex;
  std::string first_error;
  std::shared_ptr<ibis::util::ThreadBudget> budget = ibis::util::ThreadBudget::current();
  auto worker = [&](size_t id) {
    ibis::util::ThreadBudgetScope scope(budget);
    try {
      for(;;) {
        const size_t k = next.fetch_add(1);
        if(k >= work.size() || failed.load()) break;
        if(thd_killed(thd)) {
          failed = true;
          break;
        }
        std::string message;
        std::vector<warp_pa_worker> local;
        if(!warp_pa_partition(plan, work[k], snap, &local, &message)) {
          std::lock_guard<std::mutex> guard(error_mutex);
          if(first_error.empty()) first_error = message;
          failed = true;
          break;
        }
        if(plan.overflow) {
          failed = true;
          break;
        }
        warp_pa_merge_into(plan, &shared, &local);
        warp_release_partition(work[k]->currentDataDir());
      }
    } catch(const std::exception &e) {
      std::lock_guard<std::mutex> guard(error_mutex);
      if(first_error.empty()) first_error = e.what();
      failed = true;
    } catch(...) {
      std::lock_guard<std::mutex> guard(error_mutex);
      if(first_error.empty()) first_error = "unknown exception";
      failed = true;
    }
  };
  std::vector<std::thread> threads;
  for(size_t id = 1; id < workers; ++id) threads.emplace_back(worker, id);
  worker(0);
  for(auto &t : threads) t.join();
  for(auto &part : parts) {
    delete part;
    part = nullptr;
  }
  if(thd_killed(thd)) {
    *error = "interrupted";
    return false;
  }
  if(plan.overflow) {
    *error = "BIGINT value is out of range in a SUM";
    return false;
  }
  if(!first_error.empty()) {
    *error = first_error;
    return false;
  }

  warp_pa_collect(plan, shared);
  return true;
}

static std::string warp_pa_decimal_text(warp_i128 value, int scale) {
  const bool negative = value < 0;
  unsigned __int128 magnitude = negative ? -(unsigned __int128)value : (unsigned __int128)value;
  char digits[64];
  int n = 0;
  do {
    digits[n++] = '0' + (char)(magnitude % 10);
    magnitude /= 10;
  } while(magnitude != 0);
  while(n <= scale) digits[n++] = '0';   // at least one digit before the point
  std::string text;
  if(negative) text.push_back('-');
  for(int i = n - 1; i >= 0; --i) {
    text.push_back(digits[i]);
    if(i == scale && scale > 0) text.push_back('.');
  }
  return text;
}

static inline bool warp_pa_compare(Item_func::Functype op, int cmp) {
  switch(op) {
    case Item_func::GT_FUNC: return cmp > 0;
    case Item_func::GE_FUNC: return cmp >= 0;
    case Item_func::LT_FUNC: return cmp < 0;
    case Item_func::LE_FUNC: return cmp <= 0;
    case Item_func::EQ_FUNC: return cmp == 0;
    default: return cmp != 0;
  }
}

/* does the group pass the conditions of HAVING that the engine knows? An
   aggregate that is NULL does not pass (a comparison with NULL is not true). */
static bool warp_pa_having_passes(const warp_pa_plan &plan, const warp_pa_group &group) {
  for(const warp_pa_having &h : plan.having) {
    const warp_pa_agg &agg = plan.aggs[h.agg];
    const warp_pa_state &st = group.states[h.agg];
    int cmp;
    if(agg.kind == warp_pa_agg::COUNT_ALL || agg.kind == warp_pa_agg::COUNT_COL) {
      const warp_i128 n = agg.kind == warp_pa_agg::COUNT_ALL ? (warp_i128)group.rows : (warp_i128)st.count;
      cmp = n < h.ivalue ? -1 : (n > h.ivalue ? 1 : 0);
    } else if(agg.result_kind == PA_REAL) {
      if(agg.kind == warp_pa_agg::SUM ? st.count == 0 : !st.have) return false;
      const double d = agg.kind == warp_pa_agg::SUM ? st.dsum : st.dval;
      cmp = d < h.dvalue ? -1 : (d > h.dvalue ? 1 : 0);
    } else {
      if(agg.kind == warp_pa_agg::SUM ? st.count == 0 : !st.have) return false;
      warp_i128 v = agg.kind == warp_pa_agg::SUM ? st.isum : st.ival;
      warp_i128 c = h.ivalue;
      const int vs = agg.result_kind == PA_DEC ? agg.result_scale : 0;
      if(vs < h.iscale) v *= warp_pa_pow10(h.iscale - vs);
      else if(vs > h.iscale) c *= warp_pa_pow10(vs - h.iscale);
      cmp = v < c ? -1 : (v > c ? 1 : 0);
    }
    if(!warp_pa_compare(h.op, cmp)) return false;
  }
  return true;
}

static void warp_pa_to_decimal(my_decimal *out, warp_i128 value, int scale) {
  const std::string text = warp_pa_decimal_text(value, scale);
  /* the end of the text goes in, the end of the number comes out */
  const char *parsed_end = text.c_str() + text.size();
  str2my_decimal(E_DEC_FATAL_ERROR, text.c_str(), out, &parsed_end);
}

/* ---------------------------------------------------------------------- */
/* the handler                                                            */
/* ---------------------------------------------------------------------- */

int ha_warp::pa_run() {
  warp_pa_plan &plan = *par_agg;
  warp_index_build_scope budget_scope(ha_thd(), thread_budget);
  plan.where = push_where_clause == "1=1" ? "" : push_where_clause;
  /* the keys of another table that limit the scan (see warp_try_semi_keys) */
  plan.semi_active = false;
  plan.semi_slot = -1;
  semi_pick();
  if(semi_in) {
    for(size_t i = 0; i < plan.cols.size(); ++i) {
      if(plan.cols[i].field == semi_in->inner_col && plan.cols[i].kind == PA_INT &&
         std::find(plan.key_cols.begin(), plan.key_cols.end(), (int)i) != plan.key_cols.end()) {
        plan.semi_slot = (int)i;
      }
    }
    if(plan.semi_slot >= 0) {
      /* a bit for every key: a row is looked up in it without parsing a list */
      std::vector<int64_t> keys;
      if(semi_key_values(&keys, std::min<uint64_t>(table_row_count() / 2, 200000000ULL))) {
        if(keys.empty()) {
          plan.semi_range = 0;
          plan.semi_bits.clear();
          plan.semi_active = true;
        } else if((uint64_t)(keys.back() - keys.front()) < (1ULL << 31)) {
          plan.semi_min = keys.front();
          plan.semi_range = (uint64_t)(keys.back() - keys.front()) + 1;
          plan.semi_bits.assign((plan.semi_range + 63) / 64, 0);
          for(int64_t key : keys) {
            const uint64_t i = (uint64_t)(key - plan.semi_min);
            plan.semi_bits[i >> 6] |= 1ULL << (i & 63);
          }
          plan.semi_active = true;
        }
      }
    } else {
      const std::string cond = semi_key_condition();
      if(cond != "") {
        plan.where = plan.where == "" ? cond : "(" + plan.where + ") AND (" + cond + ")";
      }
    }
  }
  plan.where_fields.clear();
  for(size_t i = 0; i + 1 < plan.where.size(); ++i) {
    const std::string &w = plan.where;
    if(w[i] == 'c' && (i == 0 || !isalnum((unsigned char)w[i - 1])) && isdigit((unsigned char)w[i + 1])) {
      uint f = 0;
      size_t j = i + 1;
      while(j < w.size() && isdigit((unsigned char)w[j])) {
        f = f * 10 + (w[j] - '0');
        ++j;
      }
      if(std::find(plan.where_fields.begin(), plan.where_fields.end(), f) == plan.where_fields.end()) {
        plan.where_fields.push_back(f);
      }
    }
  }
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  try {
    if(!warp_pa_execute(plan, &error)) {
      if(thd_killed(plan.thd)) {
        plan.thd->send_kill_message();
        return HA_ERR_QUERY_INTERRUPTED;
      }
      if(error.compare(0, 6, "BIGINT") == 0) {
        my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "BIGINT", error.c_str());
        return HA_ERR_INTERNAL_ERROR;
      }
      sql_print_error("WARP: parallel aggregation of %s failed: %s.  The FastBit cache "
                      "(warp_cache_size = %llu bytes) may be too small for this query.",
                      share->data_dir_name, error.c_str(), (unsigned long long)my_cache_size);
      return HA_ERR_OUT_OF_MEM;
    }
  } catch(const std::bad_alloc &) {
    sql_print_error("WARP: the parallel aggregation of %s ran out of memory", share->data_dir_name);
    return HA_ERR_OUT_OF_MEM;
  } catch(const std::exception &e) {
    sql_print_error("WARP: parallel aggregation of %s failed: %s", share->data_dir_name, e.what());
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return HA_ERR_INTERNAL_ERROR;
  }
  plan.computed = true;
  plan.pos = 0;
  if(getenv("WARP_PA_TRACE") != nullptr) {
    sql_print_warning("WARP: parallel aggregation of %s: %zu groups in %.3f s", share->data_dir_name,
                      plan.groups.size(),
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  }
  return 0;
}

int ha_warp::pa_next(uchar *) {
  warp_pa_plan &plan = *par_agg;
  while(plan.pos < plan.groups.size() && !plan.having.empty() &&
        !warp_pa_having_passes(plan, plan.groups[plan.pos])) {
    ++plan.pos;
  }
  if(plan.pos >= plan.groups.size()) {
    warp_pa_now_plan = nullptr;
    warp_pa_now_group = nullptr;
    return HA_ERR_END_OF_FILE;
  }
  const warp_pa_group &group = plan.groups[plan.pos++];
  /* the columns of the tables of a join that MySQL reads above the aggregation */
  for(size_t k = 0; k < plan.outs.size() && k < group.cells.size(); ++k) {
    const warp_pa_out &o = plan.outs[k];
    const warp_pa_cell &cell = group.cells[k];
    TABLE *out_table = o.table;
    my_bitmap_map *old_out = tmp_use_all_columns(out_table, out_table->write_set);
    Field *field = o.f;
    if(cell.null) {
      field->set_null();
    } else {
      field->set_notnull();
      switch(o.kind) {
        case WARP_GJ_INT: {
          const enum_field_types type = field->real_type();
          if(type == MYSQL_TYPE_DATE || type == MYSQL_TYPE_NEWDATE || type == MYSQL_TYPE_DATETIME ||
             type == MYSQL_TYPE_DATETIME2) {
            MYSQL_TIME ltime;
            TIME_from_longlong_datetime_packed(&ltime, cell.i);
            field->store_time(&ltime, field->decimals());
          } else {
            field->store((longlong)cell.i, field->is_flag_set(UNSIGNED_FLAG));
          }
          break;
        }
        case WARP_GJ_DEC: {
          const std::string text = warp_pa_decimal_text(cell.i, o.scale);
          field->store(text.data(), text.size(), &my_charset_latin1);
          break;
        }
        case WARP_GJ_REAL:
          field->store(cell.d);
          break;
        default:
          field->store(cell.s.data(), cell.s.size(), field->charset());
      }
    }
    tmp_restore_column_map(out_table->write_set, old_out);
  }
  /* the columns of the group in the record of the table */
  my_bitmap_map *old_write = tmp_use_all_columns(table, table->write_set);
  for(size_t k = 0; k < plan.key_cols.size(); ++k) {
    const warp_pa_col &col = plan.cols[plan.key_cols[k]];
    const warp_pa_cell &cell = group.cells[k];
    Field *field = table->field[col.field];
    if(cell.null) {
      field->set_null();
      continue;
    }
    field->set_notnull();
    switch(col.kind) {
      case PA_INT:
        field->store((longlong)cell.i, col.is_unsigned);
        break;
      case PA_DEC: {
        const std::string text = warp_pa_decimal_text(cell.i, col.scale);
        field->store(text.data(), text.size(), &my_charset_latin1);
        break;
      }
      case PA_REAL:
        field->store(cell.d);
        break;
      default:
        field->store(cell.s.data(), cell.s.size(), col.cs);
    }
  }
  tmp_restore_column_map(table->write_set, old_write);
  /* the constants that replace the arguments of the aggregates */
  for(size_t m = 0; m < plan.aggs.size(); ++m) {
    warp_pa_agg &agg = plan.aggs[m];
    const warp_pa_state &st = group.states[m];
    if(!agg.replaced) {
      continue;
    }
    switch(agg.kind) {
      case warp_pa_agg::COUNT_ALL:
      case warp_pa_agg::COUNT_COL:
        break;  // warp_pa_count adds the count of the row
      case warp_pa_agg::SUM:
      case warp_pa_agg::AVG:
        if(agg.result_kind == PA_REAL) {
          agg.real_holder->value = st.dsum;
          agg.real_holder->null_value = st.count == 0;
        } else {
          my_decimal decimal;
          warp_pa_to_decimal(&decimal, st.count == 0 ? 0 : st.isum, agg.result_scale);
          agg.dec_holder->set_decimal_value(&decimal);
          agg.dec_holder->null_value = st.count == 0;
        }
        break;
      case warp_pa_agg::MIN:
      case warp_pa_agg::MAX:
        if(agg.result_kind == PA_STR) {
          agg.str_holder->assign(st.sval, !st.have);
        } else if(agg.result_kind == PA_REAL) {
          agg.real_holder->value = st.dval;
          agg.real_holder->null_value = !st.have;
        } else if(agg.result_kind == PA_DEC) {
          my_decimal decimal;
          warp_pa_to_decimal(&decimal, st.have ? st.ival : 0, agg.result_scale);
          agg.dec_holder->set_decimal_value(&decimal);
          agg.dec_holder->null_value = !st.have;
        } else {
          agg.int_holder->value = st.have ? (longlong)st.ival : 0;
          agg.int_holder->null_value = !st.have;
        }
        break;
    }
  }
  warp_pa_now_plan = &plan;
  warp_pa_now_group = &group;
  return 0;
}

/* Called from push_to_engine: changes the plan if the engine can aggregate. */
static bool warp_pa_install(THD *thd, std::shared_ptr<warp_pa_plan> plan, AccessPath **slot, AccessPath **top_slot,
                            AccessPath *stream);

static bool warp_pa_replace(THD *thd, AccessPath *agg, const JOIN *join) {
  /* COUNT(DISTINCT) is aggregated over sorted rows that a STREAM path copies
     into a temporary table */
  AccessPath **const top_slot = agg->type == AccessPath::AGGREGATE ? &agg->aggregate().child
                                                                   : &agg->temptable_aggregate().subquery_path;
  AccessPath **slot = top_slot;
  AccessPath *stream = nullptr;
  bool sorted = false;
  int chain_streams = 0;
  while((*slot)->type == AccessPath::SORT || (*slot)->type == AccessPath::STREAM) {
    if((*slot)->type == AccessPath::SORT) {
      sorted = true;
      slot = &(*slot)->sort().child;
    } else {
      stream = *slot;
      ++chain_streams;
      slot = &(*slot)->stream().child;
    }
  }
  if(chain_streams > 1) {
    return warp_pa_no(__LINE__);
  }
  std::shared_ptr<warp_pa_plan> plan = warp_pa_make_plan(thd, agg, slot, join, stream, sorted);
  if(!plan) {
    return false;
  }
  return warp_pa_install(thd, plan, slot, top_slot, stream);
}

/* The scan of the table of the plan replaces the tree below the aggregation:
   the aggregates get the constants of the groups. */
static bool warp_pa_install(THD *thd, std::shared_ptr<warp_pa_plan> plan, AccessPath **slot, AccessPath **top_slot,
                            AccessPath *stream) {
  /* the fields that the engine sets must be in the read set */
  for(int slot_index : plan->key_cols) {
    bitmap_set_bit(plan->table->read_set, plan->cols[slot_index].field);
  }
  /* the constants that replace the arguments, the classes of COUNT and AVG */
  for(auto &a : plan->aggs) {
    if(!a.replaced) {
      continue;
    }
    switch(a.kind) {
      case warp_pa_agg::COUNT_ALL:
      case warp_pa_agg::COUNT_COL:
        warp_pa_adopt_count(thd, a.item);
        break;
      case warp_pa_agg::SUM:
      case warp_pa_agg::AVG:
        if(a.result_kind == PA_REAL) {
          a.real_holder = new (thd->mem_root) Item_float(0.0, 0);
          a.item->set_arg(thd, 0, a.real_holder);
        } else {
          a.dec_holder = new (thd->mem_root) Item_decimal((longlong)0, false);
          a.item->set_arg(thd, 0, a.dec_holder);
        }
        if(a.kind == warp_pa_agg::AVG) {
          warp_pa_adopt_avg(thd, a.item);
        }
        break;
      case warp_pa_agg::MIN:
      case warp_pa_agg::MAX: {
        Item *argument = a.item->get_arg(0);
        if(a.result_kind == PA_STR) {
          a.str_holder = new (thd->mem_root) warp_pa_string("", 0, argument->collation.collation);
          a.str_holder->collation = argument->collation;
          a.item->set_arg(thd, 0, a.str_holder);
        } else if(a.result_kind == PA_REAL) {
          a.real_holder = new (thd->mem_root) Item_float(0.0, argument->decimals);
          a.item->set_arg(thd, 0, a.real_holder);
        } else if(a.result_kind == PA_DEC) {
          a.dec_holder = new (thd->mem_root) Item_decimal((longlong)0, false);
          a.item->set_arg(thd, 0, a.dec_holder);
        } else {
          a.int_holder = new (thd->mem_root) Item_int((longlong)0);
          a.int_holder->unsigned_flag = argument->unsigned_flag;
          a.item->set_arg(thd, 0, a.int_holder);
        }
        break;
      }
    }
  }
  AccessPath *scan = NewTableScanAccessPath(thd, plan->table, false);
  scan->set_num_output_rows((*slot)->num_output_rows());
  scan->set_cost((*slot)->cost());
  /* the sort that precedes the aggregation of MySQL is not needed: the groups
     are next to each other */
  *(plan->adjacent && stream == nullptr ? top_slot : slot) = scan;
  plan->handler->par_agg = plan;
  return true;
}

/* ---------------------------------------------------------------------- */
/* an aggregation of the rows of the join that the engine makes           */
/* ---------------------------------------------------------------------- */

static bool warp_pja_int_field(const Field *f) {
  switch(f->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      return true;
    case MYSQL_TYPE_LONGLONG:
      return !f->is_flag_set(UNSIGNED_FLAG);
    default:
      return false;
  }
}

/* COUNT, SUM, MIN and MAX of integer columns of the tables of the join, grouped
   by integer columns of its probe table.  agg_path is the aggregation, slot the
   tree of the join: nested loops, one for every table that the join reads
   (they return the rows that the engine chose), over the scan of the probe
   table. */
static std::shared_ptr<warp_pa_plan> warp_pja_make_plan(THD *thd, AccessPath *agg_path, AccessPath **slot,
                                                        const JOIN *join, AccessPath *stream, bool sorted) {
  std::shared_ptr<warp_pa_plan> none;
  if(stream != nullptr) {
    return (warp_pa_no(__LINE__), none);
  }
  /* the tree of the join that warp_pj_replace made */
  AccessPath *node = *slot;
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
    if(!found) return (warp_pa_no(__LINE__), none);
  }
  auto level_of = [&](const Field *field) -> int {
    if(field->table == probe) return 0;
    for(int l = 1; l <= pj->nbuild; ++l) {
      if(pj->tables[l].table == field->table) return l;
    }
    return -1;
  };

  /* GROUP BY: integer columns of the probe table */
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
  auto plan = std::make_shared<warp_pa_plan>();
  plan->thd = thd;
  plan->table = probe;
  plan->handler = handler;
  std::vector<Field *> group_fields;
  for(; group != nullptr; group = group->next) {
    Item *item = (*group->item)->real_item();
    if(item->type() != Item::FIELD_ITEM) {
      return (warp_pa_no(__LINE__), none);
    }
    Field *field = down_cast<Item_field *>(item)->field;
    if(field == nullptr || field->table != probe || !warp_pja_int_field(field)) {
      return (warp_pa_no(__LINE__), none);
    }
    const int col = warp_pa_add_col(plan.get(), field);
    if(col < 0 || plan->cols[col].kind != PA_INT) {
      return (warp_pa_no(__LINE__), none);
    }
    if(std::find(plan->key_cols.begin(), plan->key_cols.end(), col) == plan->key_cols.end()) {
      plan->key_cols.push_back(col);
      group_fields.push_back(field);
    }
  }
  plan->group_cols = plan->key_cols.size();
  if(plan->group_cols == 0) {
    return (warp_pa_no(__LINE__), none);
  }

  /* the aggregates */
  if(join->sum_funcs == nullptr || *join->sum_funcs == nullptr) {
    return (warp_pa_no(__LINE__), none);
  }
  std::vector<std::pair<Field *, int>> arg_fields;   // the column of an aggregate, its table
  for(Item_sum **it = join->sum_funcs; *it != nullptr; ++it) {
    Item_sum *sum = *it;
    warp_pa_agg agg;
    agg.item = sum;
    agg.replaced = true;
    Field *arg_field = nullptr;
    if(sum->has_with_distinct() || sum->argument_count() != 1) {
      return (warp_pa_no(__LINE__), none);
    }
    Item *arg = sum->get_arg(0);
    const Item_sum::Sumfunctype type = sum->sum_func();
    if(type == Item_sum::COUNT_FUNC && (arg->type() == Item::INT_ITEM || arg->const_item())) {
      agg.kind = warp_pa_agg::COUNT_ALL;
    } else {
      Item *real = arg->real_item();
      if(real->type() != Item::FIELD_ITEM) {
        return (warp_pa_no(__LINE__), none);
      }
      arg_field = down_cast<Item_field *>(real)->field;
      if(arg_field == nullptr || level_of(arg_field) < 0 || !warp_pja_int_field(arg_field)) {
        return (warp_pa_no(__LINE__), none);
      }
      switch(type) {
        case Item_sum::COUNT_FUNC:
          agg.kind = warp_pa_agg::COUNT_COL;
          break;
        case Item_sum::SUM_FUNC:
          if(sum->result_type() != DECIMAL_RESULT) return (warp_pa_no(__LINE__), none);
          agg.kind = warp_pa_agg::SUM;
          agg.result_kind = PA_DEC;
          agg.result_scale = 0;
          break;
        case Item_sum::MIN_FUNC:
        case Item_sum::MAX_FUNC:
          if(sum->result_type() != INT_RESULT) return (warp_pa_no(__LINE__), none);
          agg.kind = type == Item_sum::MIN_FUNC ? warp_pa_agg::MIN : warp_pa_agg::MAX;
          agg.result_kind = PA_INT;
          break;
        default:
          return (warp_pa_no(__LINE__), none);
      }
    }
    plan->aggs.push_back(agg);
    arg_fields.push_back({arg_field, arg_field == nullptr ? 0 : level_of(arg_field)});
  }

  /* nothing else of the tables is read above the aggregation: the columns of
     the select list and of HAVING that are not in an aggregate are columns of
     the groups */
  bool other_columns = false;
  auto check = [&](Item *item) {
    WalkItem(item, enum_walk::PREFIX, [&](Item *i) {
      if(i->type() == Item::SUM_FUNC_ITEM) {
        return true;   // its arguments are replaced
      }
      if(i->type() == Item::FIELD_ITEM) {
        Field *f = down_cast<Item_field *>(i)->field;
        if(f == nullptr || level_of(f) < 0) return false;   // a column of another table
        if(std::find(group_fields.begin(), group_fields.end(), f) == group_fields.end()) other_columns = true;
      }
      return false;
    });
  };
  if(join->fields != nullptr) {
    for(Item *item : *join->fields) check(item);
  }
  if(join->having_cond != nullptr) check(join->having_cond);
  if(other_columns || join->having_cond != nullptr) {
    return (warp_pa_no(__LINE__), none);
  }

  /* the columns that the join reads for the aggregation */
  auto sink = std::make_shared<warp_pja_sink>();
  sink->pa = plan.get();
  for(Field *field : group_fields) {
    warp_pj_ref ref;
    ref.level = 0;
    ref.slot = warp_pj_add_root(*pj, field->field_index());
    sink->group.push_back(ref);
  }
  for(size_t m = 0; m < plan->aggs.size(); ++m) {
    warp_pja_agg a;
    a.kind = plan->aggs[m].kind;
    if(arg_fields[m].first != nullptr) {
      const int level = arg_fields[m].second;
      a.ref.level = level;
      a.ref.slot = level == 0 ? warp_pj_add_root(*pj, arg_fields[m].first->field_index())
                              : warp_pj_add_value(*pj, level, arg_fields[m].first->field_index());
    }
    sink->aggs.push_back(a);
  }
  /* MySQL does not read the rows of the other tables: only the columns of the
     keys, of the aggregates and of the conditions that MySQL still evaluates
     are read from them, and no image of the rows is kept */
  for(int l = 1; l <= pj->nbuild; ++l) {
    warp_pj_table &t = pj->tables[l];
    std::set<uint> keep(t.value_fields.begin(), t.value_fields.end());
    for(Item *filter : t.filters) {
      WalkItem(filter, enum_walk::PREFIX, [&](Item *i) {
        if(i->type() == Item::FIELD_ITEM && down_cast<Item_field *>(i)->field != nullptr) {
          keep.insert(down_cast<Item_field *>(i)->field->field_index());
        }
        return false;
      });
    }
    for(Field **field = t.table->field; *field; ++field) {
      if(keep.count((*field)->field_index()) == 0) {
        bitmap_clear_bit(t.table->read_set, (*field)->field_index());
      }
    }
    t.ranges.clear();
    t.image_len = 0;
  }
  plan->adjacent = sorted;
  plan->pj = pj;
  pj->sink = sink;
  return plan;
}

static bool warp_pja_replace(THD *thd, AccessPath *agg, const JOIN *join) {
  AccessPath **const top_slot = agg->type == AccessPath::AGGREGATE ? &agg->aggregate().child
                                                                   : &agg->temptable_aggregate().subquery_path;
  AccessPath **slot = top_slot;
  bool sorted = false;
  while((*slot)->type == AccessPath::SORT) {
    sorted = true;
    slot = &(*slot)->sort().child;
  }
  std::shared_ptr<warp_pa_plan> plan = warp_pja_make_plan(thd, agg, slot, join, nullptr, sorted);
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
  return warp_pa_install(thd, plan, slot, top_slot, nullptr);
}

/* The aggregations of the rows of the joins that the engine makes (called after
   warp_try_parallel_join) */
static bool warp_gja_replace(THD *thd, AccessPath *agg, const JOIN *join);

static void warp_try_parallel_join_aggregation(THD *thd, AccessPath *root, JOIN *join) {
  if(!THDVAR(thd, parallel_aggregation) || !THDVAR(thd, parallel_hash_join) ||
     (thd->lex->sql_command != SQLCOM_SELECT && thd->lex->sql_command != SQLCOM_CREATE_TABLE &&
      thd->lex->sql_command != SQLCOM_INSERT_SELECT)) {
    return;
  }
  WalkAccessPaths(root, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [&](AccessPath *path, const JOIN *block_join) {
                    if(path->type != AccessPath::AGGREGATE && path->type != AccessPath::TEMPTABLE_AGGREGATE) {
                      return false;
                    }
                    if(warp_block_is_executed_again(block_join) || block_join->query_block->has_windows()) {
                      return false;
                    }
                    if(!warp_gja_replace(thd, path, block_join)) {
                      warp_pja_replace(thd, path, block_join);
                    }
                    return false;
                  });
}

static void warp_try_parallel_aggregation(THD *thd, AccessPath *root, JOIN *join) {
  if(!THDVAR(thd, parallel_aggregation) ||
     (thd->lex->sql_command != SQLCOM_SELECT && thd->lex->sql_command != SQLCOM_CREATE_TABLE &&
      thd->lex->sql_command != SQLCOM_INSERT_SELECT)) {
    return;
  }
  WalkAccessPaths(root, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [&](AccessPath *path, const JOIN *block_join) {
                    if(path->type == AccessPath::MATERIALIZE) {
                      if(!warp_block_is_executed_again(block_join)) {
                        warp_pa_replace_distinct(thd, path);
                      }
                      return false;
                    }
                    if(path->type != AccessPath::AGGREGATE && path->type != AccessPath::TEMPTABLE_AGGREGATE) {
                      return false;
                    }
                    /* a block that is executed again and again would aggregate every time */
                    if(warp_block_is_executed_again(block_join) || block_join->query_block->has_windows()) {
                      warp_pa_no(__LINE__);
                      return false;
                    }
                    warp_pa_replace(thd, path, block_join);
                    return false;
                  });
}

#endif
