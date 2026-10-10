/*
  Aggregation of star schema queries inside the engine.

  A query such as

    SELECT d_year, s_city, SUM(lo_revenue - lo_supplycost)
      FROM lineorder JOIN dim_date ON lo_orderdatekey = d_datekey
                     JOIN supplier ON lo_suppkey = s_suppkey
     WHERE s_nation = 'UNITED STATES' AND d_year = 1997 GROUP BY d_year, s_city

  is normally executed by MySQL: the engine returns the rows of the fact table
  that pass the pushed down conditions and MySQL joins them to the dimension
  tables and aggregates them, in one thread.  Here the engine does all of it:

   * The dimension tables are read with their pushed down conditions into
     lookup tables: join key -> group code, where a group code stands for one
     distinct combination of the GROUP BY columns of that dimension.
   * Every partition of the fact table is processed by a worker thread (the
     partitions are independent): the pushed down conditions are evaluated
     with the bitmap indexes, the join keys are looked up, the SUM
     expressions are evaluated and added to the sum of the group.
   * The sums of the workers are merged.

  MySQL gets one row per group.  The plan that push_to_engine receives is
  changed: the join below the aggregation is replaced by a scan of the fact
  table whose rows are the groups.  A row is made of the record images of the
  dimension rows of the group (the GROUP BY items read the fields of the
  dimension tables) and of the sums, which are given to the SUM items through
  constants that replace their arguments.  MySQL aggregates those rows again
  (every group is a row, SUM can be added up), applies HAVING, ORDER BY and
  LIMIT, so what the engine has to know is limited to the join and the sums.

  The query is only changed when the engine can do exactly what MySQL would:
  see warp_star_plan for what is accepted.  This file is included by
  ha_warp.cc, the plugin is a single translation unit.
*/

#ifndef WARP_STAR_AGG_H
#define WARP_STAR_AGG_H

#include <sys/stat.h>
#include <atomic>
#include <thread>
#include <unordered_map>

#include "sql-common/my_decimal.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/relational_expression.h"

typedef __int128 warp_i128;

/* WARP_STAR_TRACE=1 in the environment of the server: the line of the check
   that kept the engine from aggregating a query is written to the error log */
static inline bool warp_star_no(int line) {
  if(getenv("WARP_STAR_TRACE") != nullptr) {
    sql_print_warning("WARP: star aggregation not used, warp_star_agg.h:%d", line);
  }
  return false;
}

/* a dimension table of the star */
struct warp_star_dim {
  TABLE *table = nullptr;
  ha_warp *handler = nullptr;
  uint fact_key = 0;                // field index of the join column in the fact table
  uint dim_key = 0;                 // field index of the join column in the dimension
  std::vector<uint> group_fields;   // GROUP BY columns of this dimension

  /* built from the rows of the dimension that pass its conditions */
  std::vector<std::string> images;  // record image of one row of every group code
  uint32_t ncodes = 1;              // number of group codes
  uint64_t stride = 1;              // weight of the code in the group number
  static const uint32_t MULTI = 0x80000000U;
  std::vector<uint32_t> dense;      // code + 1 of the key - dense_min (0: no row) or MULTI | index
  int64_t dense_min = 0;
  uint64_t dense_range = 0;
  std::unordered_map<int64_t, uint32_t> sparse;  // same for keys that do not fit a dense table
  std::vector<std::vector<uint32_t>> multi;      // the codes of a key that has several rows
  bool has_multi = false;

  /* The code of a key: 0 if the key has no row (not a match), code + 1, or
     MULTI | index of the list of codes. */
  inline uint32_t lookup(int64_t key) const {
    if(dense_range != 0) {
      const uint64_t k = (uint64_t)key - (uint64_t)dense_min;
      return k < dense_range ? dense[k] : 0;
    }
    auto it = sparse.find(key);
    return it == sparse.end() ? 0 : it->second;
  }
};

/* one operation of the postfix form of a SUM argument */
struct warp_star_op {
  enum Kind { COLUMN, CONSTANT, ADD, SUB, MUL } kind;
  int slot = 0;           // COLUMN: position in warp_star_agg::fact_cols
  int null_slot = -1;     // COLUMN: position in warp_star_agg::null_cols, -1 if not nullable
  int64_t value = 0;      // CONSTANT
};

struct warp_star_sum {
  Item_sum_sum *item = nullptr;
  Item_decimal *holder = nullptr;   // replaces the argument of the SUM
  std::vector<warp_star_op> ops;
  std::vector<int> null_slots;      // the NULL markers that make the argument NULL
  std::string text;                 // for error messages
};

/* the values of a column of one partition */
struct warp_star_col {
  int type = -1;
  size_t n = 0;
  ibis::array_t<signed char> i8;
  ibis::array_t<unsigned char> u8;
  ibis::array_t<int16_t> i16;
  ibis::array_t<uint16_t> u16;
  ibis::array_t<int32_t> i32;
  ibis::array_t<uint32_t> u32;
  ibis::array_t<int64_t> i64;

  bool load(const ibis::column *col, size_t nrows) {
    if(col == nullptr) {
      return false;
    }
    type = (int)col->type();
    int rc = -1;
    switch(col->type()) {
      case ibis::BYTE:   rc = col->getValuesArray(&i8);  n = i8.size();  break;
      case ibis::UBYTE:  rc = col->getValuesArray(&u8);  n = u8.size();  break;
      case ibis::SHORT:  rc = col->getValuesArray(&i16); n = i16.size(); break;
      case ibis::USHORT: rc = col->getValuesArray(&u16); n = u16.size(); break;
      case ibis::INT:    rc = col->getValuesArray(&i32); n = i32.size(); break;
      case ibis::UINT:   rc = col->getValuesArray(&u32); n = u32.size(); break;
      case ibis::LONG:   rc = col->getValuesArray(&i64); n = i64.size(); break;
      default: return false;
    }
    return rc == 0 && n == nrows;
  }
  inline int64_t get(size_t i) const {
    switch(type) {
      case ibis::BYTE:   return i8[i];
      case ibis::UBYTE:  return u8[i];
      case ibis::SHORT:  return i16[i];
      case ibis::USHORT: return u16[i];
      case ibis::INT:    return i32[i];
      case ibis::UINT:   return u32[i];
      default:           return i64[i];
    }
  }
};

/* Sums per group; a flat array for few groups, a hash table for many.  The
   block of a group is: the sums, the number of values that were not NULL
   for every sum, the number of rows. */
struct warp_star_acc {
  size_t nsums = 0;
  uint64_t groups = 0;
  bool flat = true;
  std::vector<warp_i128> block;                                  // flat
  std::unordered_map<uint64_t, std::vector<warp_i128>> hash;     // not flat
  bool overflow = false;
  size_t overflow_sum = 0;

  inline size_t width() const { return 2 * nsums + 1; }
  void init(size_t nsums_arg, uint64_t groups_arg) {
    nsums = nsums_arg;
    groups = groups_arg;
    flat = groups_arg * (2 * nsums_arg + 1) <= (1ULL << 20);
    if(flat) {
      block.assign(groups * width(), 0);
    }
  }
  /* the block of the group, the row is counted */
  inline warp_i128 *row(uint64_t gid) {
    warp_i128 *b;
    if(flat) {
      b = &block[gid * width()];
    } else {
      auto &v = hash[gid];
      if(v.empty()) {
        v.assign(width(), 0);
      }
      b = v.data();
    }
    b[2 * nsums] += 1;
    return b;
  }
  void merge(warp_star_acc &other) {
    if(other.overflow) {
      overflow = true;
      overflow_sum = other.overflow_sum;
    }
    if(flat) {
      for(size_t i = 0; i < block.size(); ++i) {
        block[i] += other.block[i];
      }
    } else {
      for(auto &entry : other.hash) {
        auto &v = hash[entry.first];
        if(v.empty()) {
          v.assign(width(), 0);
        }
        for(size_t i = 0; i < width(); ++i) {
          v[i] += entry.second[i];
        }
      }
    }
  }
};

/* what a transaction may see, taken once for the whole query */
struct warp_star_snapshot {
  uint64_t trx_id = 0;
  bool repeatable_read = false;   // REPEATABLE READ or SERIALIZABLE
  bool check_rowids = false;      // rows may be deleted or have history locks
};

struct warp_star_result_row {
  std::vector<uint32_t> codes;    // group code of every dimension
  std::vector<warp_i128> sums;
  std::vector<uint8_t> is_null;   // the sum is NULL: all its values were NULL
};

struct warp_star_agg {
  THD *thd = nullptr;
  TABLE *fact_table = nullptr;
  ha_warp *fact_handler = nullptr;
  std::vector<warp_star_dim> dims;
  std::vector<warp_star_sum> sums;
  std::vector<uint> fact_cols;    // fields of the fact table read from every partition
  std::vector<uint> null_cols;    // fields whose NULL markers are read
  std::vector<int> key_slot;      // position of the join column of every dimension in fact_cols
  std::string fact_where;         // pushed down conditions of the fact table (FastBit syntax)
  std::vector<uint> where_fields; // fields in fact_where

  /* the result */
  std::vector<warp_star_result_row> rows;
  size_t pos = 0;
  bool computed = false;
};

/* ---------------------------------------------------------------------- */
/* the part of the work that does not depend on the data                  */
/* ---------------------------------------------------------------------- */

/* Postfix form of a SUM argument made of integer columns of the fact table,
   integer constants and + - *.  The result is what MySQL computes with
   BIGINT arithmetic. */
static bool warp_star_compile(Item *item, TABLE *fact, std::vector<uint> *cols,
                              std::vector<uint> *null_cols, std::vector<warp_star_op> *ops) {
  if(item->type() == Item::FIELD_ITEM) {
    Field *field = down_cast<Item_field *>(item)->field;
    if(field == nullptr || field->table != fact) {
      return warp_star_no(__LINE__);
    }
    switch(field->real_type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
        break;
      case MYSQL_TYPE_LONGLONG:
        if(field->is_flag_set(UNSIGNED_FLAG)) {
          return warp_star_no(__LINE__);
        }
        break;
      default:
        return warp_star_no(__LINE__);
    }
    const uint index = field->field_index();
    size_t slot = 0;
    while(slot < cols->size() && (*cols)[slot] != index) {
      ++slot;
    }
    if(slot == cols->size()) {
      cols->push_back(index);
    }
    warp_star_op op;
    op.kind = warp_star_op::COLUMN;
    op.slot = (int)slot;
    if(field->is_nullable()) {
      size_t n = 0;
      while(n < null_cols->size() && (*null_cols)[n] != index) {
        ++n;
      }
      if(n == null_cols->size()) {
        null_cols->push_back(index);
      }
      op.null_slot = (int)n;
    }
    ops->push_back(op);
    return true;
  }
  if(item->type() == Item::INT_ITEM) {
    if(item->unsigned_flag && (ulonglong)item->val_int() > (ulonglong)INT64_MAX) {
      return warp_star_no(__LINE__);
    }
    warp_star_op op;
    op.kind = warp_star_op::CONSTANT;
    op.value = item->val_int();
    ops->push_back(op);
    return true;
  }
  if(item->type() == Item::FUNC_ITEM && item->result_type() == INT_RESULT) {
    Item_func *func = down_cast<Item_func *>(item);
    warp_star_op::Kind kind;
    switch(func->functype()) {
      case Item_func::PLUS_FUNC:  kind = warp_star_op::ADD; break;
      case Item_func::MINUS_FUNC: kind = warp_star_op::SUB; break;
      case Item_func::MUL_FUNC:   kind = warp_star_op::MUL; break;
      default: return warp_star_no(__LINE__);
    }
    if(func->argument_count() != 2) {
      return warp_star_no(__LINE__);
    }
    if(!warp_star_compile(func->arguments()[0], fact, cols, null_cols, ops) ||
       !warp_star_compile(func->arguments()[1], fact, cols, null_cols, ops)) {
      return warp_star_no(__LINE__);
    }
    warp_star_op op;
    op.kind = kind;
    ops->push_back(op);
    return true;
  }
  return warp_star_no(__LINE__);
}

/* The tables and the join conditions below an aggregation.  Only inner hash
   joins whose conditions are equalities of two columns are accepted. */
static bool warp_star_collect(AccessPath *path, std::vector<TABLE *> *tables,
                              std::vector<Item_eq_base *> *conditions) {
  switch(path->type) {
    case AccessPath::TABLE_SCAN: {
      TABLE *table = path->table_scan().table;
      if(table == nullptr || dynamic_cast<ha_warp *>(table->file) == nullptr) {
        return warp_star_no(__LINE__);
      }
      tables->push_back(table);
      return true;
    }
    case AccessPath::HASH_JOIN: {
      const JoinPredicate *predicate = path->hash_join().join_predicate;
      if(predicate == nullptr || predicate->expr == nullptr ||
         predicate->expr->type != RelationalExpression::INNER_JOIN ||
         !predicate->expr->join_conditions.empty() ||
         path->hash_join().rewrite_semi_to_inner) {
        return warp_star_no(__LINE__);
      }
      for(Item_eq_base *condition : predicate->expr->equijoin_conditions) {
        conditions->push_back(condition);
      }
      return warp_star_collect(path->hash_join().outer, tables, conditions) &&
             warp_star_collect(path->hash_join().inner, tables, conditions);
    }
    default:
      /* a FILTER is a condition that the engine did not take over */
      return warp_star_no(__LINE__);
  }
}

static bool warp_star_int_key(Field *field, bool may_be_null) {
  if(field == nullptr || (field->is_nullable() && !may_be_null)) {
    return warp_star_no(__LINE__);
  }
  switch(field->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      return true;
    case MYSQL_TYPE_LONGLONG:
      return !field->is_flag_set(UNSIGNED_FLAG);
    default:
      return warp_star_no(__LINE__);
  }
}

/* Is the query below the aggregation a star join that the engine can
   evaluate?  If so, the plan is made and returned. */
static std::shared_ptr<warp_star_agg> warp_star_make_plan(THD *thd, AccessPath *agg_path,
                                                          AccessPath **child_slot,
                                                          JOIN *join) {
  std::shared_ptr<warp_star_agg> none;

  std::vector<TABLE *> tables;
  std::vector<Item_eq_base *> conditions;
  if(!warp_star_collect(*child_slot, &tables, &conditions)) {
    return (warp_star_no(__LINE__), none);
  }
  if(tables.size() < 2) {
    return (warp_star_no(__LINE__), none);
  }

  /* the fact table is the largest one */
  TABLE *fact = nullptr;
  for(TABLE *table : tables) {
    auto info = get_pushdown_info(thd, table->alias);
    if(info != nullptr && info->is_fact_table) {
      if(fact != nullptr) {
        return (warp_star_no(__LINE__), none);
      }
      fact = table;
    }
  }
  if(fact == nullptr) {
    return (warp_star_no(__LINE__), none);
  }

  /* reads that lock rows are done by the normal scan */
  for(TABLE *table : tables) {
    ha_warp *ha = dynamic_cast<ha_warp *>(table->file);
    if(ha->locks_rows_on_read()) {
      return (warp_star_no(__LINE__), none);
    }
  }
  {
    warp_trx *trx = warp_get_trx(dynamic_cast<ha_warp *>(fact->file)->ht, thd);
    if(trx != nullptr && (trx->isolation_level == ISO_SERIALIZABLE || trx->lock_in_share_mode)) {
      return (warp_star_no(__LINE__), none);
    }
  }

  auto plan = std::make_shared<warp_star_agg>();
  plan->thd = thd;
  plan->fact_table = fact;
  plan->fact_handler = dynamic_cast<ha_warp *>(fact->file);

  /* every other table is joined to the fact table by one column */
  for(TABLE *table : tables) {
    if(table == fact) {
      continue;
    }
    warp_star_dim dim;
    dim.table = table;
    dim.handler = dynamic_cast<ha_warp *>(table->file);
    bool found = false;
    for(Item_eq_base *condition : conditions) {
      Item *left = condition->get_arg(0)->real_item();
      Item *right = condition->get_arg(1)->real_item();
      if(left->type() != Item::FIELD_ITEM || right->type() != Item::FIELD_ITEM) {
        return (warp_star_no(__LINE__), none);
      }
      Field *lf = down_cast<Item_field *>(left)->field;
      Field *rf = down_cast<Item_field *>(right)->field;
      if(lf == nullptr || rf == nullptr) {
        return (warp_star_no(__LINE__), none);
      }
      Field *fact_field = nullptr, *dim_field = nullptr;
      if(lf->table == fact && rf->table == table) {
        fact_field = lf;
        dim_field = rf;
      } else if(rf->table == fact && lf->table == table) {
        fact_field = rf;
        dim_field = lf;
      } else {
        continue;
      }
      if(found) {
        return (warp_star_no(__LINE__), none);  // two conditions between the same tables
      }
      if(!warp_star_int_key(fact_field, false) || !warp_star_int_key(dim_field, true)) {
        return (warp_star_no(__LINE__), none);
      }
      dim.fact_key = fact_field->field_index();
      dim.dim_key = dim_field->field_index();
      found = true;
    }
    if(!found) {
      return (warp_star_no(__LINE__), none);  // not joined to the fact table (a snowflake or a cross join)
    }
    plan->dims.push_back(std::move(dim));
  }
  /* every condition joins the fact table to a dimension */
  for(Item_eq_base *condition : conditions) {
    Field *lf = down_cast<Item_field *>(condition->get_arg(0)->real_item())->field;
    Field *rf = down_cast<Item_field *>(condition->get_arg(1)->real_item())->field;
    if(lf->table != fact && rf->table != fact) {
      return (warp_star_no(__LINE__), none);
    }
  }

  /* GROUP BY: columns of the dimension tables */
  ORDER *group = nullptr;
  if(agg_path->type == AccessPath::TEMPTABLE_AGGREGATE) {
    group = agg_path->temptable_aggregate().table->group;
    if(group == nullptr) {
      return (warp_star_no(__LINE__), none);
    }
  } else {
    if(join->group_list.order != nullptr || agg_path->aggregate().olap != UNSPECIFIED_OLAP_TYPE) {
      return (warp_star_no(__LINE__), none);
    }
  }
  for(; group != nullptr; group = group->next) {
    Item *item = (*group->item)->real_item();
    if(item->type() != Item::FIELD_ITEM) {
      return (warp_star_no(__LINE__), none);
    }
    Field *field = down_cast<Item_field *>(item)->field;
    if(field == nullptr || field->table == fact) {
      return (warp_star_no(__LINE__), none);
    }
    bool placed = false;
    for(auto &dim : plan->dims) {
      if(dim.table == field->table) {
        switch(field->type()) {
          case MYSQL_TYPE_BLOB:
          case MYSQL_TYPE_TINY_BLOB:
          case MYSQL_TYPE_MEDIUM_BLOB:
          case MYSQL_TYPE_LONG_BLOB:
          case MYSQL_TYPE_JSON:
          case MYSQL_TYPE_GEOMETRY:
            return (warp_star_no(__LINE__), none);
          default:
            break;
        }
        dim.group_fields.push_back(field->field_index());
        placed = true;
        break;
      }
    }
    if(!placed) {
      return (warp_star_no(__LINE__), none);
    }
  }

  /* the aggregate functions: SUM of integer expressions over the fact table */
  if(join->sum_funcs == nullptr || *join->sum_funcs == nullptr) {
    return (warp_star_no(__LINE__), none);
  }
  for(Item_sum **it = join->sum_funcs; *it != nullptr; ++it) {
    Item_sum *sum = *it;
    if(sum->sum_func() != Item_sum::SUM_FUNC || sum->has_with_distinct() ||
       sum->argument_count() != 1 || sum->result_type() != DECIMAL_RESULT) {
      return (warp_star_no(__LINE__), none);
    }
    warp_star_sum entry;
    entry.item = down_cast<Item_sum_sum *>(sum);
    if(!warp_star_compile(sum->get_arg(0), fact, &plan->fact_cols, &plan->null_cols, &entry.ops)) {
      return (warp_star_no(__LINE__), none);
    }
    for(const auto &op : entry.ops) {
      if(op.kind == warp_star_op::COLUMN && op.null_slot >= 0 &&
         std::find(entry.null_slots.begin(), entry.null_slots.end(), op.null_slot) == entry.null_slots.end()) {
        entry.null_slots.push_back(op.null_slot);
      }
    }
    String text;
    sum->get_arg(0)->print(thd, &text, QT_ORDINARY);
    entry.text.assign(text.ptr(), text.length());
    plan->sums.push_back(std::move(entry));
  }

  /* the join columns of the fact table are read first */
  std::vector<uint> measure_cols;
  measure_cols.swap(plan->fact_cols);
  for(auto &dim : plan->dims) {
    size_t slot = 0;
    while(slot < plan->fact_cols.size() && plan->fact_cols[slot] != dim.fact_key) {
      ++slot;
    }
    if(slot == plan->fact_cols.size()) {
      plan->fact_cols.push_back(dim.fact_key);
    }
    plan->key_slot.push_back((int)slot);
  }
  /* the measure columns were numbered before the keys were added, map them */
  for(auto &sum : plan->sums) {
    for(auto &op : sum.ops) {
      if(op.kind != warp_star_op::COLUMN) {
        continue;
      }
      const uint index = measure_cols[op.slot];
      size_t slot = 0;
      while(slot < plan->fact_cols.size() && plan->fact_cols[slot] != index) {
        ++slot;
      }
      if(slot == plan->fact_cols.size()) {
        plan->fact_cols.push_back(index);
      }
      op.slot = (int)slot;
    }
  }
  return plan;
}


/* ---------------------------------------------------------------------- */
/* execution                                                              */
/* ---------------------------------------------------------------------- */

static bool warp_star_trx_visible(const warp_star_snapshot &snap, uint64_t row_trx) {
  if(snap.trx_id == row_trx) {
    return true;
  }
  /* only the rows of committed transactions are visible to others */
  if(row_trx == 0 || !warp_state->is_trx_committed(row_trx)) {
    return false;
  }
  if(row_trx < snap.trx_id) {
    return true;
  }
  /* newer: visible for READ COMMITTED and READ UNCOMMITTED only */
  return !snap.repeatable_read;
}

static bool warp_star_row_visible(const warp_star_snapshot &snap, uint64_t rowid) {
  const uint64_t history = warp_state->get_history_lock(rowid);
  if(history == 0 || history < snap.trx_id ||
     (history > snap.trx_id && !snap.repeatable_read)) {
    return !warp_state->delete_bitmap->is_set(rowid);
  }
  /* another transaction has deleted or updated the row */
  return history != snap.trx_id;
}

/* the dimension rows that pass the conditions of the dimension -> lookup */
static bool warp_star_build_dim(warp_star_dim &dim, std::string *error) {
  TABLE *table = dim.table;
  ha_warp *handler = dim.handler;
  struct entry {
    int64_t key;
    uint32_t code;
  };
  std::vector<entry> keys;
  std::unordered_map<std::string, uint32_t> codes;
  uchar *record = table->record[0];

  int rc = handler->ha_rnd_init(true);
  if(rc != 0) {
    *error = "could not scan the dimension table";
    return false;
  }
  while((rc = handler->ha_rnd_next(record)) == 0) {
    Field *key_field = table->field[dim.dim_key];
    if(key_field->is_null()) {
      continue;  // NULL does not join
    }
    uint32_t code = 0;
    if(!dim.group_fields.empty()) {
      std::string signature;
      for(uint f : dim.group_fields) {
        Field *field = table->field[f];
        if(field->is_null()) {
          signature.push_back('\1');
          continue;
        }
        signature.push_back('\0');
        StringBuffer<128> buffer;
        field->val_str(&buffer, &buffer);
        const uint32_t length = (uint32_t)buffer.length();
        signature.append((const char *)&length, sizeof(length));
        signature.append(buffer.ptr(), buffer.length());
      }
      auto found = codes.find(signature);
      if(found == codes.end()) {
        code = (uint32_t)dim.images.size();
        dim.images.emplace_back((const char *)record, table->s->reclength);
        codes.emplace(std::move(signature), code);
      } else {
        code = found->second;
      }
    }
    keys.push_back({key_field->val_int(), code});
  }
  handler->ha_rnd_end();
  if(rc != HA_ERR_END_OF_FILE) {
    *error = "could not read the dimension table";
    return false;
  }
  dim.ncodes = dim.group_fields.empty() ? 1 : (uint32_t)dim.images.size();
  if(dim.ncodes == 0) {
    dim.ncodes = 1;
  }

  /* the lookup: a table indexed by key if the keys are close together */
  auto add = [&dim](uint32_t &slot, uint32_t code) {
    if(slot == 0) {
      slot = code + 1;
    } else if(slot & warp_star_dim::MULTI) {
      dim.multi[slot & ~warp_star_dim::MULTI].push_back(code);
    } else {
      dim.multi.push_back({slot - 1, code});
      slot = warp_star_dim::MULTI | (uint32_t)(dim.multi.size() - 1);
      dim.has_multi = true;
    }
  };
  if(!keys.empty()) {
    int64_t lo = keys[0].key, hi = keys[0].key;
    for(const entry &e : keys) {
      lo = std::min(lo, e.key);
      hi = std::max(hi, e.key);
    }
    const uint64_t range = (uint64_t)hi - (uint64_t)lo + 1;
    if(range != 0 && range <= (1ULL << 27) &&
       range <= std::max<uint64_t>(keys.size() * 16, 1ULL << 20)) {
      dim.dense.assign(range, 0);
      dim.dense_min = lo;
      dim.dense_range = range;
      for(const entry &e : keys) {
        add(dim.dense[(uint64_t)e.key - (uint64_t)lo], e.code);
      }
    } else {
      for(const entry &e : keys) {
        add(dim.sparse[e.key], e.code);
      }
    }
  }
  return true;
}

/* the rows of one partition that a transaction may not see; empty if all */
static bool warp_star_invisible(ibis::part *part, const warp_star_snapshot &snap,
                                std::vector<uint8_t> *invisible) {
  invisible->clear();
  const size_t nrows = part->nRows();
  ibis::array_t<uint64_t> trx_array, rowid_array;
  const ibis::column *tcol = part->getColumn("t");
  if(tcol == nullptr || tcol->getValuesArray(&trx_array) != 0 ||
     trx_array.size() != nrows) {
    return false;
  }
  const uint64_t *trx_ids = trx_array.begin();
  const uint64_t *row_ids = nullptr;
  if(snap.check_rowids) {
    const ibis::column *rcol = part->getColumn("r");
    if(rcol == nullptr || rcol->getValuesArray(&rowid_array) != 0 ||
       rowid_array.size() != nrows) {
      return false;
    }
    row_ids = rowid_array.begin();
  }
  uint64_t previous = 0;
  bool have_previous = false, visible = false;
  for(size_t i = 0; i < nrows; ++i) {
    if(!have_previous || trx_ids[i] != previous) {
      previous = trx_ids[i];
      have_previous = true;
      visible = warp_star_trx_visible(snap, previous);
    }
    bool ok = visible;
    if(ok && row_ids != nullptr) {
      ok = warp_star_row_visible(snap, row_ids[i]);
    }
    if(!ok) {
      if(invisible->empty()) {
        invisible->assign(nrows, 0);
      }
      (*invisible)[i] = 1;
    }
  }
  return true;
}

static inline bool warp_star_eval(const warp_star_sum &sum,
                                  const std::vector<warp_star_col> &cols, size_t row,
                                  int64_t *result) {
  int64_t stack[16];
  int top = 0;
  for(const warp_star_op &op : sum.ops) {
    switch(op.kind) {
      case warp_star_op::COLUMN:
        stack[top++] = cols[op.slot].get(row);
        break;
      case warp_star_op::CONSTANT:
        stack[top++] = op.value;
        break;
      default: {
        const int64_t b = stack[--top];
        const int64_t a = stack[--top];
        int64_t r;
        bool overflow;
        if(op.kind == warp_star_op::ADD) {
          overflow = __builtin_add_overflow(a, b, &r);
        } else if(op.kind == warp_star_op::SUB) {
          overflow = __builtin_sub_overflow(a, b, &r);
        } else {
          overflow = __builtin_mul_overflow(a, b, &r);
        }
        if(overflow) {
          return false;
        }
        stack[top++] = r;
      }
    }
  }
  *result = stack[0];
  return true;
}

/* one partition of the fact table */
static bool warp_star_partition(warp_star_agg &plan, ibis::part *part,
                                const warp_star_snapshot &snap, warp_star_acc *acc,
                                std::string *error) {
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

  /* the rows that pass the conditions of the fact table */
  ibis::bitvector everything;
  std::unique_ptr<ibis::query> query;
  const ibis::bitvector *hits = nullptr;
  if(plan.fact_where.empty() || plan.fact_where == "1=1") {
    everything.set(1, nrows);
    hits = &everything;
  } else {
    query.reset(new ibis::query((const char *)0, part, (const char *)0));
    query->addConditions(plan.fact_where.c_str());
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

  std::vector<warp_star_col> cols(plan.fact_cols.size());
  for(size_t k = 0; k < plan.fact_cols.size(); ++k) {
    const std::string name = "c" + std::to_string(plan.fact_cols[k]);
    if(!cols[k].load(part->getColumn(name.c_str()), nrows)) {
      *error = std::string("could not read column ") + name + " of " + dir;
      return false;
    }
  }

  std::vector<warp_star_col> nulls(plan.null_cols.size());
  for(size_t k = 0; k < plan.null_cols.size(); ++k) {
    const std::string name = "n" + std::to_string(plan.null_cols[k]);
    if(!nulls[k].load(part->getColumn(name.c_str()), nrows)) {
      *error = std::string("could not read column ") + name + " of " + dir;
      return false;
    }
  }

  const size_t ndims = plan.dims.size();
  const size_t nsums = plan.sums.size();
  bool has_multi = false;
  for(const auto &dim : plan.dims) {
    has_multi = has_multi || dim.has_multi;
  }
  std::vector<int64_t> values(nsums);

  /* adds the row to the groups; a key with several dimension rows makes
     several groups */
  std::vector<std::vector<uint32_t>> choices(ndims);
  auto add_row = [&](size_t row, uint64_t gid) -> bool {
    warp_i128 *block = acc->row(gid);
    for(size_t m = 0; m < nsums; ++m) {
      /* a NULL makes the argument NULL, SUM does not add it */
      bool is_null = false;
      for(int ns : plan.sums[m].null_slots) {
        if(nulls[ns].get(row) != 0) {
          is_null = true;
          break;
        }
      }
      if(is_null) {
        continue;
      }
      int64_t value;
      if(!warp_star_eval(plan.sums[m], cols, row, &value)) {
        acc->overflow = true;
        acc->overflow_sum = m;
        return false;
      }
      block[m] += value;
      block[nsums + m] += 1;
    }
    return true;
  };
  auto process = [&](size_t row) -> bool {
    if(!invisible.empty() && invisible[row]) {
      return true;
    }
    uint64_t gid = 0;
    if(!has_multi) {
      for(size_t d = 0; d < ndims; ++d) {
        const uint32_t slot = plan.dims[d].lookup(cols[plan.key_slot[d]].get(row));
        if(slot == 0) {
          return true;
        }
        gid += (uint64_t)(slot - 1) * plan.dims[d].stride;
      }
      return add_row(row, gid);
    }
    /* generic: every combination of the matching dimension rows */
    for(size_t d = 0; d < ndims; ++d) {
      const uint32_t slot = plan.dims[d].lookup(cols[plan.key_slot[d]].get(row));
      if(slot == 0) {
        return true;
      }
      choices[d].clear();
      if(slot & warp_star_dim::MULTI) {
        choices[d] = plan.dims[d].multi[slot & ~warp_star_dim::MULTI];
      } else {
        choices[d].push_back(slot - 1);
      }
    }
    std::vector<size_t> at(ndims, 0);
    for(;;) {
      uint64_t g = 0;
      for(size_t d = 0; d < ndims; ++d) {
        g += (uint64_t)choices[d][at[d]] * plan.dims[d].stride;
      }
      if(!add_row(row, g)) {
        return false;
      }
      size_t d = 0;
      while(d < ndims && ++at[d] == choices[d].size()) {
        at[d++] = 0;
      }
      if(d == ndims) {
        break;
      }
    }
    return true;
  };

  for(ibis::bitvector::indexSet ix = hits->firstIndexSet(); ix.nIndices() > 0; ++ix) {
    const ibis::bitvector::word_t *indices = ix.indices();
    if(ix.isRange()) {
      for(ibis::bitvector::word_t row = indices[0]; row < indices[1]; ++row) {
        if(!process(row)) {
          return true;  // the overflow is in acc
        }
      }
    } else {
      for(ibis::bitvector::word_t j = 0; j < ix.nIndices(); ++j) {
        if(!process(indices[j])) {
          return true;
        }
      }
    }
  }
  return true;
}

/* Computes the groups.  Returns false and sets *error if it can not. */
static bool warp_star_execute(warp_star_agg &plan, std::string *error) {
  THD *thd = plan.thd;
  plan.rows.clear();
  plan.pos = 0;

  /* what the transaction may see */
  warp_star_snapshot snap;
  warp_trx *trx = warp_get_trx(plan.fact_handler->ht, thd);
  if(trx == nullptr) {
    *error = "no transaction";
    return false;
  }
  snap.trx_id = trx->trx_id;
  snap.repeatable_read = (trx->isolation_level == ISO_REPEATABLE_READ ||
                          trx->isolation_level == ISO_SERIALIZABLE);
  snap.check_rowids = warp_state->has_history_locks() || warp_state->delete_bitmap->may_have_bits();

  /* no row is written while the columns are read */
  warp_table_read_lock fact_lock(plan.fact_handler->get_warp_share()->data_dir_name);

  uint64_t groups = 1;
  for(auto &dim : plan.dims) {
    if(!warp_star_build_dim(dim, error)) {
      return false;
    }
    dim.stride = groups;
    if(groups > UINT64_MAX / dim.ncodes) {
      *error = "too many groups";
      return false;
    }
    groups *= dim.ncodes;
  }

  ibis::partList parts;
  ibis::util::gatherParts(parts, plan.fact_handler->get_warp_share()->data_dir_name, true);
  std::vector<ibis::part *> work;
  for(ibis::part *part : parts) {
    if(part != nullptr && part->nRows() > 0 &&
       std::string(part->currentDataDir()) !=
           std::string(plan.fact_handler->get_warp_share()->data_dir_name)) {
      work.push_back(part);
    }
  }

  /* the workers: as many as the degree of parallelism allows, and as many
     as the cache can hold (every worker has the columns of a partition) */
  size_t workers = THDVAR(thd, max_degree_of_parallelism);
  if(workers < 1) {
    workers = 1;
  }
  if(!work.empty()) {
    const uint64_t per_worker =
        std::max<uint64_t>(work[0]->nRows(), 1) * 8 * (plan.fact_cols.size() + 3);
    workers = std::min<uint64_t>(
        workers, std::max<uint64_t>(1, (ibis::fileManager::currentCacheSize() / 2) / per_worker));
  }
  workers = std::min(workers, std::max<size_t>(work.size(), 1));

  std::vector<warp_star_acc> accs(workers);
  for(auto &acc : accs) {
    acc.init(plan.sums.size(), groups);
  }
  std::atomic<size_t> next(0);
  std::atomic<bool> failed(false);
  std::mutex error_mutex;
  std::string first_error;

  auto worker = [&](size_t id) {
    try {
      for(;;) {
        const size_t k = next.fetch_add(1);
        if(k >= work.size() || failed.load()) {
          break;
        }
        std::string message;
        if(!warp_star_partition(plan, work[k], snap, &accs[id], &message)) {
          std::lock_guard<std::mutex> guard(error_mutex);
          if(first_error.empty()) {
            first_error = message;
          }
          failed = true;
          break;
        }
        if(accs[id].overflow) {
          failed = true;
          break;
        }
        warp_release_partition(work[k]->currentDataDir());
      }
    } catch(const std::exception &e) {
      std::lock_guard<std::mutex> guard(error_mutex);
      if(first_error.empty()) {
        first_error = e.what();
      }
      failed = true;
    } catch(...) {
      std::lock_guard<std::mutex> guard(error_mutex);
      if(first_error.empty()) {
        first_error = "unknown exception";
      }
      failed = true;
    }
  };
  std::vector<std::thread> threads;
  for(size_t id = 1; id < workers; ++id) {
    threads.emplace_back(worker, id);
  }
  worker(0);
  for(auto &t : threads) {
    t.join();
  }
  for(auto &part : parts) {
    delete part;
    part = nullptr;
  }

  for(size_t id = 0; id < workers; ++id) {
    if(accs[id].overflow) {
      char message[256];
      snprintf(message, sizeof(message), "BIGINT value is out of range in '%s'",
               plan.sums[accs[id].overflow_sum].text.c_str());
      *error = message;
      return false;
    }
  }
  if(!first_error.empty()) {
    *error = first_error;
    return false;
  }
  for(size_t id = 1; id < workers; ++id) {
    accs[0].merge(accs[id]);
  }

  /* one row per group that has rows */
  const size_t nsums = plan.sums.size();
  auto emit = [&](uint64_t gid, const warp_i128 *block) {
    warp_star_result_row row;
    row.codes.resize(plan.dims.size());
    for(size_t d = 0; d < plan.dims.size(); ++d) {
      row.codes[d] = (uint32_t)((gid / plan.dims[d].stride) % plan.dims[d].ncodes);
    }
    row.sums.assign(block, block + nsums);
    row.is_null.resize(nsums);
    for(size_t m = 0; m < nsums; ++m) {
      row.is_null[m] = block[nsums + m] == 0;
    }
    plan.rows.push_back(std::move(row));
  };
  if(accs[0].flat) {
    for(uint64_t g = 0; g < groups; ++g) {
      const warp_i128 *block = &accs[0].block[g * accs[0].width()];
      if(block[2 * nsums] != 0) {
        emit(g, block);
      }
    }
  } else {
    for(auto &entry : accs[0].hash) {
      emit(entry.first, entry.second.data());
    }
  }
  return true;
}

static void warp_star_set_decimal(Item_decimal *holder, warp_i128 value) {
  my_decimal decimal;
  if(value >= INT64_MIN && value <= INT64_MAX) {
    int2my_decimal(E_DEC_FATAL_ERROR, (longlong)value, false, &decimal);
  } else {
    char text[64];
    char *end = text + sizeof(text);
    char *at = end;
    const bool negative = value < 0;
    unsigned __int128 magnitude = negative ? -(unsigned __int128)value : (unsigned __int128)value;
    *--at = 0;
    do {
      *--at = '0' + (char)(magnitude % 10);
      magnitude /= 10;
    } while(magnitude != 0);
    if(negative) {
      *--at = '-';
    }
    const char *parsed_end;
    str2my_decimal(E_DEC_FATAL_ERROR, at, &decimal, &parsed_end);
  }
  holder->set_decimal_value(&decimal);
}

/* ---------------------------------------------------------------------- */
/* the handler                                                            */
/* ---------------------------------------------------------------------- */

void ha_warp::print_error(int error, myf errflag) {
  THD *thd = current_thd;
  if(thd != nullptr && thd->is_error()) {
    return;
  }
  handler::print_error(error, errflag);
}

bool ha_warp::star_agg_run() {
  warp_star_agg &plan = *star_agg;
  std::string error;
  try {
    if(!warp_star_execute(plan, &error)) {
      sql_print_error("WARP: aggregation of %s failed: %s", share->data_dir_name, error.c_str());
      if(error.compare(0, 6, "BIGINT") == 0) {
        my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "BIGINT", error.c_str());
      } else {
        my_error(ER_UNKNOWN_ERROR, MYF(0));
      }
      return true;
    }
  } catch(const std::exception &e) {
    sql_print_error("WARP: aggregation of %s failed: %s", share->data_dir_name, e.what());
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return true;
  }
  plan.computed = true;
  plan.pos = 0;
  return false;
}

int ha_warp::star_agg_next(uchar *) {
  warp_star_agg &plan = *star_agg;
  if(plan.pos >= plan.rows.size()) {
    return HA_ERR_END_OF_FILE;
  }
  const warp_star_result_row &row = plan.rows[plan.pos++];
  for(size_t d = 0; d < plan.dims.size(); ++d) {
    warp_star_dim &dim = plan.dims[d];
    if(!dim.group_fields.empty()) {
      memcpy(dim.table->record[0], dim.images[row.codes[d]].data(), dim.table->s->reclength);
    }
  }
  for(size_t m = 0; m < plan.sums.size(); ++m) {
    warp_star_set_decimal(plan.sums[m].holder, row.is_null[m] ? 0 : row.sums[m]);
    plan.sums[m].holder->null_value = row.is_null[m] != 0;
  }
  return 0;
}

/* Called from push_to_engine: changes the plan if the engine can aggregate. */
static void warp_try_star_aggregation(THD *thd, AccessPath *root, JOIN *join) {
  if(getenv("WARP_STAR_TRACE") != nullptr) {
    sql_print_warning("WARP: star aggregation check, enabled=%d command=%d",
                          (int)THDVAR(thd, star_aggregation), (int)thd->lex->sql_command);
  }
  if(!THDVAR(thd, star_aggregation) ||
     (thd->lex->sql_command != SQLCOM_SELECT && thd->lex->sql_command != SQLCOM_CREATE_TABLE &&
      thd->lex->sql_command != SQLCOM_INSERT_SELECT)) {
    return;
  }
  if(join->query_block->outer_query_block() != nullptr ||
     join->query_expression()->is_set_operation() ||
     join->query_block->has_windows()) {
    warp_star_no(__LINE__);
    return;
  }
  AccessPath *agg = nullptr;
  int aggregations = 0;
  bool other_blocks = false;
  WalkAccessPaths(root, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [&](AccessPath *path, const JOIN *) {
                    if(path->type == AccessPath::AGGREGATE ||
                       path->type == AccessPath::TEMPTABLE_AGGREGATE) {
                      agg = path;
                      ++aggregations;
                    } else if(path->type == AccessPath::MATERIALIZE ||
                              path->type == AccessPath::WINDOW ||
                              path->type == AccessPath::STREAM) {
                      other_blocks = true;
                    }
                    return false;
                  });
  if(aggregations != 1 || other_blocks) {
    warp_star_no(__LINE__);
    return;
  }
  AccessPath **slot = agg->type == AccessPath::AGGREGATE
                          ? &agg->aggregate().child
                          : &agg->temptable_aggregate().subquery_path;
  std::shared_ptr<warp_star_agg> plan = warp_star_make_plan(thd, agg, slot, join);
  if(!plan) {
    return;
  }

  /* the fields the engine reads and sets must be in the read set */
  for(auto &dim : plan->dims) {
    bitmap_set_bit(dim.table->read_set, dim.dim_key);
    for(uint f : dim.group_fields) {
      bitmap_set_bit(dim.table->read_set, f);
    }
  }
  plan->fact_where = plan->fact_handler->push_where_clause;
  /* the columns in the condition, "c<N>" */
  for(size_t i = 0; i + 1 < plan->fact_where.size(); ++i) {
    if(plan->fact_where[i] == 'c' && (i == 0 || !isalnum((unsigned char)plan->fact_where[i - 1]) ) &&
       isdigit((unsigned char)plan->fact_where[i + 1])) {
      uint n = 0;
      size_t j = i + 1;
      while(j < plan->fact_where.size() && isdigit((unsigned char)plan->fact_where[j])) {
        n = n * 10 + (plan->fact_where[j] - '0');
        ++j;
      }
      if(std::find(plan->where_fields.begin(), plan->where_fields.end(), n) == plan->where_fields.end()) {
        plan->where_fields.push_back(n);
      }
    }
  }

  /* SUM(a) becomes the sum of the sums of the groups: the argument is a
     constant that the handler sets to the sum of the group */
  for(auto &sum : plan->sums) {
    sum.holder = new (thd->mem_root) Item_decimal((longlong)0, false);
    sum.item->set_arg(thd, 0, sum.holder);
  }

  AccessPath *scan = NewTableScanAccessPath(thd, plan->fact_table, false);
  scan->set_num_output_rows((*slot)->num_output_rows());
  scan->set_cost((*slot)->cost());
  *slot = scan;
  plan->fact_handler->star_agg = plan;
}

#endif
