/*
  Parallel hash join of WARP tables inside the engine.

  MySQL joins the tables of a query one row at a time in one thread: it reads
  the rows of every table through the handler, builds a hash table from one
  input of a join and probes it with the rows of the other one.  A query that
  joins a large table (LINEITEM of TPC-H) to several others spends nearly all of
  its time in that single thread.  Here the engine joins the tables:

   * Every table but the largest one is read through its handler (with the
     conditions that were pushed down and the visibility rules of the
     transaction).  The columns that the join keys are made of are kept as
     arrays of integers, the row is kept as a compact image, and a hash table
     is made on the join key.
   * The partitions of the largest table (the probe table) are processed by
     worker threads: the pushed down conditions are evaluated with the bitmap
     indexes, the rows that the transaction can see are looked up in the hash
     tables, one table after the other, along the tree of the join conditions.
     A row that has no match in a table is dropped (inner join); a row that has
     several matches makes several result rows.
   * The result rows are returned by the handler of the probe table: the row
     of the probe table is read from its partition, the images of the other
     tables are copied into their records, which MySQL reads through the fields
     of those tables.  MySQL does the rest (conditions that are not
     equalities of two columns, aggregation, sorting).

  The plan that push_to_engine receives is changed: the join is replaced by a
  scan of the probe table (with a FILTER above it for the conditions that the
  engine does not evaluate).  It is only changed when the engine can do exactly
  what MySQL would, see warp_pj_collect and warp_pj_make_plan for what is
  accepted.  This file is included by ha_warp.cc, the plugin is a single
  translation unit.
*/

#ifndef WARP_PAR_JOIN_H
#define WARP_PAR_JOIN_H

#include <array>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/relational_expression.h"

static inline bool warp_pj_no(int line) {
  if(getenv("WARP_PJ_TRACE") != nullptr) {
    sql_print_warning("WARP: parallel hash join not used, warp_par_join.h:%d", line);
  }
  return false;
}

static const int WARP_PJ_MAX_LEVELS = 16;  // tables of one join, the probe table included
static const int WARP_PJ_MAX_KEY = 4;      // columns of a join key
static const uint32_t WARP_PJ_NULL_ROW = 0xFFFFFFFFU;  // a row of the null supplying table of a left join without a match

struct warp_pj_key {
  int64_t v[WARP_PJ_MAX_KEY] = {0, 0, 0, 0};
  bool operator==(const warp_pj_key &o) const {
    return memcmp(v, o.v, sizeof(v)) == 0;
  }
};
struct warp_pj_key_hash {
  size_t operator()(const warp_pj_key &k) const {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for(int i = 0; i < WARP_PJ_MAX_KEY; ++i) {
      h = (h ^ (uint64_t)k.v[i]) * 0xFF51AFD7ED558CCDULL;
      h ^= h >> 32;
    }
    return (size_t)h;
  }
};

/* A hash table made of the rows of a table: join key -> the rows that have
   it.  The rows of a key are chained: head gives the first row + 1 (0 is
   none), next[row] the next one + 1. */
struct warp_pj_hash {
  int width = 1;
  std::vector<uint32_t> next;
  /* a key of one column that is in a small range: a table indexed by key */
  std::vector<uint32_t> dense;
  int64_t dense_min = 0;
  uint64_t dense_range = 0;
  std::unordered_map<int64_t, uint32_t> sparse;
  std::unordered_map<warp_pj_key, uint32_t, warp_pj_key_hash> composite;

  inline uint32_t head(const int64_t *key) const {
    if(width == 1) {
      if(dense_range != 0) {
        const uint64_t k = (uint64_t)key[0] - (uint64_t)dense_min;
        return k < dense_range ? dense[k] : 0;
      }
      auto it = sparse.find(key[0]);
      return it == sparse.end() ? 0 : it->second;
    }
    warp_pj_key k;
    for(int i = 0; i < width; ++i) k.v[i] = key[i];
    auto it = composite.find(k);
    return it == composite.end() ? 0 : it->second;
  }

  /* keys[row * width + i] is the i-th column of the key of the row */
  void build(const std::vector<int64_t> &keys, size_t nrows, int width_arg) {
    width = width_arg;
    next.assign(nrows, 0);
    if(nrows == 0) {
      return;
    }
    if(width == 1) {
      int64_t lo = keys[0], hi = keys[0];
      for(size_t r = 1; r < nrows; ++r) {
        lo = std::min(lo, keys[r]);
        hi = std::max(hi, keys[r]);
      }
      const uint64_t range = (uint64_t)hi - (uint64_t)lo + 1;
      if(range != 0 && range <= (1ULL << 27) &&
         range <= std::max<uint64_t>(nrows * 16, 1ULL << 20)) {
        dense.assign(range, 0);
        dense_min = lo;
        dense_range = range;
        for(size_t r = 0; r < nrows; ++r) {
          uint32_t &slot = dense[(uint64_t)keys[r] - (uint64_t)lo];
          next[r] = slot;
          slot = (uint32_t)r + 1;
        }
      } else {
        sparse.reserve(nrows);
        for(size_t r = 0; r < nrows; ++r) {
          uint32_t &slot = sparse[keys[r]];
          next[r] = slot;
          slot = (uint32_t)r + 1;
        }
      }
      return;
    }
    composite.reserve(nrows);
    for(size_t r = 0; r < nrows; ++r) {
      warp_pj_key k;
      for(int i = 0; i < width; ++i) k.v[i] = keys[r * width + i];
      uint32_t &slot = composite[k];
      next[r] = slot;
      slot = (uint32_t)r + 1;
    }
  }
};

/* A column of a table of the join whose values the probing needs */
struct warp_pj_ref {
  int level = 0;   // 0: the probe table
  int slot = 0;    // probe table: position in warp_pj_plan::root_cols, other tables: in warp_pj_table::vals
};

/* A join condition between two tables that are not neighbours in the tree of
   the join (the tables are joined by other conditions too): it is checked when
   both rows are known. */
struct warp_pj_extra {
  warp_pj_ref a, b;
};

/* The way to a table: the key is made of columns of the parent */
struct warp_pj_edge {
  int parent = 0;
  int width = 0;
  int slot[WARP_PJ_MAX_KEY] = {0, 0, 0, 0};   // in the parent (see warp_pj_ref)
};

/* a table that is joined to the probe table, directly or through others */
struct warp_pj_table {
  TABLE *table = nullptr;
  ha_warp *handler = nullptr;
  warp_pj_edge edge;
  /* The table is the inner table of a LEFT JOIN: a row of its parent that has
     no match is returned with a row of NULLs.  The conditions of the join
     that are about this table only are checked when its rows are read. */
  bool null_side = false;
  std::vector<Item *> filters;
  std::string pushed_where;           // the conditions that FastBit evaluates when the table is read
  std::vector<std::pair<uint, const std::unordered_set<int64_t> *>> semi;   // column, the values that have a match
  std::vector<uint> own_key_fields;   // columns of this table in the key of its edge
  std::vector<uint> value_fields;     // columns kept as integers
  std::vector<std::vector<int64_t>> vals;
  std::vector<std::vector<uint8_t>> nulls;
  /* what MySQL reads: the null bytes and the columns of the read set */
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  uint32_t image_len = 0;
  std::vector<uchar> arena;           // image_len bytes per row
  std::vector<uint64_t> rowids;       // the row ids of the rows (position() of the handler)
  size_t nrows = 0;
  warp_pj_hash hash;
  uint64_t estimate = 0;              // rows of the table, for ordering

  int slot_of(uint field) const {
    for(size_t i = 0; i < value_fields.size(); ++i) {
      if(value_fields[i] == field) return (int)i;
    }
    return -1;
  }
};

/* the result rows of a partition of the probe table */
struct warp_pj_result {
  std::string dir;
  size_t stride = 0;
  std::vector<uint32_t> data;     // stride values per row: ordinal of the probe row, rows of the other tables
  std::vector<int64_t> keys;      // plan.export_slot: the value of the column for every row
  size_t rows() const { return stride == 0 ? 0 : data.size() / stride; }
};

/* An inner join of the block with a derived table (a subquery that
   MySQL materializes once) on a column of one of the tables: before the tables
   are read, the engine materializes it and drops the rows of the table whose
   column has no match, which MySQL would do for every row of the join result
   (the join of MySQL stays, this only makes the engine's result smaller). */
struct warp_pj_semi {
  AccessPath *materialize = nullptr;
  JOIN *join = nullptr;
  Field *outer = nullptr;       // the column of the table of the join
  Field *inner = nullptr;       // the column of the derived table
  int level = 0;                // the table of the plan that the column belongs to
  std::unordered_set<int64_t> keys;
};

/* Where the rows of the result of the join go instead of the result: an
   aggregation of the rows (see warp_par_agg.h).  A slice of the rows of a
   partition has its own local state. */
struct warp_pj_prober;
struct warp_pj_sink {
  virtual ~warp_pj_sink() {}
  virtual void *begin() = 0;
  virtual void add(void *local, const warp_pj_prober &prober) = 0;
  virtual void end(void *local) = 0;
};

struct warp_pj_plan {
  THD *thd = nullptr;
  TABLE *probe = nullptr;
  ha_warp *probe_handler = nullptr;
  std::vector<warp_pj_table> tables;     // [0] is the probe table (only table and handler are set)
  std::vector<std::vector<warp_pj_extra>> extras;   // by the level that completes them
  std::vector<uint> root_cols;           // columns of the probe table that are read from every partition
  std::vector<int> root_null_slot;       // position of the NULL markers in root_nulls, -1: not nullable
  std::vector<uint> root_nulls;
  std::string probe_where;               // pushed down conditions of the probe table
  std::vector<uint> where_fields;
  int nbuild = 0;                        // tables[1..nbuild] are joined to the probe table
  std::vector<warp_pj_semi> semis;

  /* the result */
  std::vector<warp_pj_result> results;
  bool computed = false;
  /* The values of a column of the probe table in the result rows are kept, for
     the scans of other tables that are limited to them (see
     ha_warp::pj_collect_keys): the join is made before the tables of those
     scans are read. */
  int export_slot = -1;              // the column in root_cols, none if < 0
  std::shared_ptr<warp_pj_sink> sink;
  bool prefetched = false;           // the join was made, pj_run does not make it again
  bool export_ready = false;
  std::vector<int64_t> export_keys;  // sorted and distinct
  /* the failure is that the FastBit cache is too small for the columns that
     the join reads */
  std::atomic<bool> out_of_cache{false};

  /* the tables that follow the probe table (see ha_warp::pj_follower) read
     their row from here once the join is made */
  bool serving = false;
  uint32_t current[WARP_PJ_MAX_LEVELS] = {0};

  /* the scan that returns it */
  size_t out_part = 0;
  size_t out_pos = 0;
  ibis::table *out_base = nullptr;
  ibis::table *out_filtered = nullptr;
  ibis::table::cursor *out_cursor = nullptr;

  void close_output() {
    delete out_cursor;
    delete out_filtered;
    delete out_base;
    out_cursor = nullptr;
    out_filtered = nullptr;
    out_base = nullptr;
  }
  ~warp_pj_plan() { close_output(); }
};

/* ---------------------------------------------------------------------- */
/* the part of the work that does not depend on the data                  */
/* ---------------------------------------------------------------------- */

static bool warp_pj_int_key(Field *field) {
  if(field == nullptr) {
    return warp_pj_no(__LINE__);
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
      return warp_pj_no(__LINE__);
  }
}

typedef std::pair<Field *, Field *> warp_pj_pair;

/* an equality of two columns of two tables; null_table is the inner table of
   the LEFT JOIN that the equality is a condition of, if it is */
struct warp_pj_pair_info {
  Field *a = nullptr;
  Field *b = nullptr;
  TABLE *null_table = nullptr;
  Item *item = nullptr;      // the condition (of a filter), to leave it to MySQL
};

struct warp_pj_collector {
  std::vector<TABLE *> tables;
  std::vector<warp_pj_pair_info> pairs;       // the join conditions
  std::vector<warp_pj_pair_info> candidates;  // equalities of filters, join keys if no outer join is involved
  std::vector<Item *> residual;               // conditions that MySQL evaluates for the result rows
  std::set<TABLE *> null_side;
  std::map<TABLE *, std::vector<Item *>> leaf_filters;  // conditions of the null side tables
};

/* An integer column that the engine can join on, without a trace */
static bool warp_pj_int_field(const Field *field) {
  if(field == nullptr) {
    return false;
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
      return false;
  }
}

static bool warp_pj_add_equality(Item_eq_base *condition, warp_pj_collector &collector, TABLE *null_table) {
  if(condition->functype() != Item_func::EQ_FUNC) {
    return warp_pj_no(__LINE__);  // <=> is NULL safe
  }
  Item *left = condition->get_arg(0)->real_item();
  Item *right = condition->get_arg(1)->real_item();
  if(left->type() != Item::FIELD_ITEM || right->type() != Item::FIELD_ITEM) {
    return warp_pj_no(__LINE__);
  }
  Field *lf = down_cast<Item_field *>(left)->field;
  Field *rf = down_cast<Item_field *>(right)->field;
  if(lf == nullptr || rf == nullptr || lf->table == rf->table) {
    return warp_pj_no(__LINE__);
  }
  warp_pj_pair_info pair;
  pair.a = lf;
  pair.b = rf;
  pair.null_table = null_table;
  collector.pairs.push_back(pair);
  return true;
}

/* A condition of the block that MySQL has not made a join condition of (the
   FILTER of an inner table of a nested loop, for example): an equality of two
   integer columns of two tables is a candidate for a join key, the tables of
   the block can be joined along it (see warp_pj_make_plan).  The other
   conditions are evaluated by MySQL for the result rows.  A condition joined
   by AND is taken apart. */
static void warp_pj_add_filter(Item *item, warp_pj_collector &collector) {
  if(item == nullptr) {
    return;
  }
  if(item->type() == Item::COND_ITEM &&
     down_cast<Item_cond *>(item)->functype() == Item_func::COND_AND_FUNC) {
    List_iterator_fast<Item> it(*down_cast<Item_cond *>(item)->argument_list());
    for(Item *part = it++; part != nullptr; part = it++) {
      warp_pj_add_filter(part, collector);
    }
    return;
  }
  if(item->type() == Item::FUNC_ITEM &&
     down_cast<Item_func *>(item)->functype() == Item_func::EQ_FUNC) {
    Item_func *eq = down_cast<Item_func *>(item);
    Item *left = eq->arguments()[0]->real_item();
    Item *right = eq->arguments()[1]->real_item();
    if(left->type() == Item::FIELD_ITEM && right->type() == Item::FIELD_ITEM) {
      Field *lf = down_cast<Item_field *>(left)->field;
      Field *rf = down_cast<Item_field *>(right)->field;
      if(lf != nullptr && rf != nullptr && lf->table != rf->table &&
         warp_pj_int_field(lf) && warp_pj_int_field(rf)) {
        warp_pj_pair_info pair;
        pair.a = lf;
        pair.b = rf;
        pair.item = item;
        collector.candidates.push_back(pair);
        return;
      }
    }
  }
  collector.residual.push_back(item);
}

/* The tables of the table map of the condition: is it about this table only? */
static bool warp_pj_only_table(Item *condition, TABLE *table) {
  const table_map used = condition->used_tables() & ~(OUTER_REF_TABLE_BIT | RAND_TABLE_BIT);
  return used == table->pos_in_table_list->map() && !condition->has_subquery() &&
         !condition->has_stored_program();
}

/* The inner side of a LEFT JOIN: one table, read with its conditions (the part
   of a condition of the join that the engine did not push down is evaluated
   when the rows are read, before the join) */
static bool warp_pj_collect_null_side(AccessPath *path, warp_pj_collector &collector) {
  std::vector<Item *> filters;
  while(path->type == AccessPath::FILTER) {
    filters.push_back(path->filter().condition);
    path = path->filter().child;
  }
  TABLE *table = nullptr;
  if(path->type == AccessPath::TABLE_SCAN) {
    table = path->table_scan().table;
  } else if(path->type == AccessPath::REF || path->type == AccessPath::EQ_REF) {
    table = path->type == AccessPath::REF ? path->ref().table : path->eq_ref().table;
    Index_lookup *ref = path->type == AccessPath::REF ? path->ref().ref : path->eq_ref().ref;
    if(table == nullptr || ref == nullptr || ref->key_parts != 1) {
      return warp_pj_no(__LINE__);
    }
    Item *value = ref->items[0]->real_item();
    if(value->type() != Item::FIELD_ITEM) {
      return warp_pj_no(__LINE__);
    }
    Field *outer_field = down_cast<Item_field *>(value)->field;
    Field *key_field = table->key_info[ref->key].key_part[0].field;
    if(outer_field == nullptr || key_field == nullptr || outer_field->table == table) {
      return warp_pj_no(__LINE__);
    }
    warp_pj_pair_info pair;
    pair.a = outer_field;
    pair.b = key_field;
    pair.null_table = table;
    collector.pairs.push_back(pair);
  } else {
    return warp_pj_no(__LINE__);
  }
  if(table == nullptr || dynamic_cast<ha_warp *>(table->file) == nullptr) {
    return warp_pj_no(__LINE__);
  }
  for(Item *filter : filters) {
    if(!warp_pj_only_table(filter, table)) {
      return warp_pj_no(__LINE__);
    }
  }
  collector.tables.push_back(table);
  collector.null_side.insert(table);
  collector.leaf_filters[table] = filters;
  return true;
}

/* The tables, the equalities between two columns and the other conditions of a
   tree of joins.  Inner joins: a FILTER in the tree (the part of a condition
   that the engine did not take over) or a condition that is not an equality
   is not given up: it is evaluated by MySQL for the result rows, which is the
   same for inner joins.  A LEFT JOIN whose inner side is one table is joined
   by the engine too (the table that has no match gets a row of NULLs). */
static bool warp_pj_collect(AccessPath *path, warp_pj_collector &collector) {
  switch(path->type) {
    case AccessPath::TABLE_SCAN: {
      TABLE *table = path->table_scan().table;
      if(table == nullptr || dynamic_cast<ha_warp *>(table->file) == nullptr) {
        return warp_pj_no(__LINE__);
      }
      collector.tables.push_back(table);
      return true;
    }
    case AccessPath::REF:
    case AccessPath::EQ_REF: {
      TABLE *table = path->type == AccessPath::REF ? path->ref().table : path->eq_ref().table;
      Index_lookup *ref = path->type == AccessPath::REF ? path->ref().ref : path->eq_ref().ref;
      if(table == nullptr || ref == nullptr || ref->key_parts != 1 ||
         dynamic_cast<ha_warp *>(table->file) == nullptr) {
        return warp_pj_no(__LINE__);
      }
      Item *value = ref->items[0]->real_item();
      if(value->type() != Item::FIELD_ITEM) {
        return warp_pj_no(__LINE__);
      }
      Field *outer_field = down_cast<Item_field *>(value)->field;
      Field *key_field = table->key_info[ref->key].key_part[0].field;
      if(outer_field == nullptr || key_field == nullptr || outer_field->table == table) {
        return warp_pj_no(__LINE__);
      }
      collector.tables.push_back(table);
      warp_pj_pair_info pair;
      pair.a = outer_field;
      pair.b = key_field;
      collector.pairs.push_back(pair);
      return true;
    }
    case AccessPath::FILTER:
      warp_pj_add_filter(path->filter().condition, collector);
      return warp_pj_collect(path->filter().child, collector);
    case AccessPath::NESTED_LOOP_JOIN: {
      const auto &join = path->nested_loop_join();
      if(join.join_type != JoinType::INNER && join.join_type != JoinType::OUTER) {
        return warp_pj_no(__LINE__);
      }
      const bool outer_join = join.join_type == JoinType::OUTER;
      if(outer_join) {
        if(!warp_pj_collect(join.outer, collector)) {
          return false;
        }
        TABLE *null_table = nullptr;
        const size_t before = collector.tables.size();
        if(!warp_pj_collect_null_side(join.inner, collector)) {
          return false;
        }
        null_table = collector.tables[before];
        /* the conditions of the join are the ones of the inner table's filters
           (the reference of an index lookup was added above) */
        if(join.join_predicate != nullptr && join.join_predicate->expr != nullptr) {
          const RelationalExpression *expr = join.join_predicate->expr;
          for(Item *condition : expr->join_conditions) {
            if(!warp_pj_only_table(condition, null_table)) {
              return warp_pj_no(__LINE__);
            }
            collector.leaf_filters[null_table].push_back(condition);
          }
          for(Item_eq_base *condition : expr->equijoin_conditions) {
            if(!warp_pj_add_equality(condition, collector, null_table)) {
              return false;
            }
          }
        }
        return true;
      }
      if(join.join_predicate != nullptr && join.join_predicate->expr != nullptr) {
        const RelationalExpression *expr = join.join_predicate->expr;
        for(Item *condition : expr->join_conditions) {
          warp_pj_add_filter(condition, collector);
        }
        for(Item_eq_base *condition : expr->equijoin_conditions) {
          if(!warp_pj_add_equality(condition, collector, nullptr)) {
            return false;
          }
        }
      }
      return warp_pj_collect(join.outer, collector) && warp_pj_collect(join.inner, collector);
    }
    case AccessPath::HASH_JOIN: {
      const JoinPredicate *predicate = path->hash_join().join_predicate;
      if(predicate == nullptr || predicate->expr == nullptr ||
         (predicate->expr->type != RelationalExpression::INNER_JOIN &&
          predicate->expr->type != RelationalExpression::LEFT_JOIN) ||
         path->hash_join().rewrite_semi_to_inner) {
        return warp_pj_no(__LINE__);
      }
      if(predicate->expr->type == RelationalExpression::LEFT_JOIN) {
        if(!warp_pj_collect(path->hash_join().outer, collector)) {
          return false;
        }
        const size_t before = collector.tables.size();
        if(!warp_pj_collect_null_side(path->hash_join().inner, collector)) {
          return false;
        }
        TABLE *null_table = collector.tables[before];
        for(Item *condition : predicate->expr->join_conditions) {
          if(!warp_pj_only_table(condition, null_table)) {
            return warp_pj_no(__LINE__);
          }
          collector.leaf_filters[null_table].push_back(condition);
        }
        for(Item_eq_base *condition : predicate->expr->equijoin_conditions) {
          if(!warp_pj_add_equality(condition, collector, null_table)) {
            return false;
          }
        }
        return true;
      }
      for(Item *condition : predicate->expr->join_conditions) {
        warp_pj_add_filter(condition, collector);
      }
      for(Item_eq_base *condition : predicate->expr->equijoin_conditions) {
        if(!warp_pj_add_equality(condition, collector, nullptr)) {
          return false;
        }
      }
      return warp_pj_collect(path->hash_join().outer, collector) &&
             warp_pj_collect(path->hash_join().inner, collector);
    }
    default:
      return warp_pj_no(__LINE__);
  }
}

/* Can MySQL read the column from a compact image of the record?  A BLOB or
   JSON value is a pointer to memory of the scan. */
static bool warp_pj_copyable(Field *field) {
  switch(field->real_type()) {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_GEOMETRY:
      return false;
    default:
      return !(field->all_flags() & BLOB_FLAG);
  }
}

static uint64_t warp_pj_rows_of(ha_warp *handler) {
  try {
    warp_table_read_lock lock(handler->get_warp_share()->data_dir_name);
    return warp_table_estimate(handler->get_warp_share()->data_dir_name, "");
  } catch(...) {
    return 0;
  }
}

/* The plan for the tree of joins of path, or none. */
static std::shared_ptr<warp_pj_plan> warp_pj_make_plan(THD *thd, AccessPath *path,
                                                       std::vector<Item *> *residual) {
  std::shared_ptr<warp_pj_plan> none;
  warp_pj_collector collector;
  if(!warp_pj_collect(path, collector)) {
    return none;
  }
  std::vector<TABLE *> &tables = collector.tables;
  /* the equalities of the filters join the tables of the block, unless a table
     of an outer join is one of them (MySQL evaluates the condition: it is
     about rows that may have NULLs) */
  std::vector<warp_pj_pair_info> pairs = collector.pairs;
  *residual = collector.residual;
  for(const warp_pj_pair_info &candidate : collector.candidates) {
    if(collector.null_side.count(candidate.a->table) || collector.null_side.count(candidate.b->table)) {
      residual->push_back(candidate.item);
    } else {
      pairs.push_back(candidate);
    }
  }
  if(tables.size() < 2 || tables.size() > (size_t)WARP_PJ_MAX_LEVELS) {
    return (warp_pj_no(__LINE__), none);
  }
  for(size_t i = 0; i < tables.size(); ++i) {
    for(size_t j = i + 1; j < tables.size(); ++j) {
      if(tables[i] == tables[j]) {
        return (warp_pj_no(__LINE__), none);  // the same table twice (a REF below a scan)
      }
    }
  }
  const uint64_t min_rows = THDVAR(thd, parallel_join_min_rows);

  /* the probe table is the largest one that is not the inner table of a LEFT
     JOIN (the rows of the outer table are all returned) */
  std::vector<uint64_t> rows(tables.size());
  size_t probe_index = tables.size();
  uint64_t largest = 0;
  for(size_t i = 0; i < tables.size(); ++i) {
    ha_warp *ha = dynamic_cast<ha_warp *>(tables[i]->file);
    if(ha->locks_rows_on_read() || ha->is_partition_child) {
      return (warp_pj_no(__LINE__), none);
    }
    rows[i] = warp_pj_rows_of(ha);
    largest = std::max(largest, rows[i]);
    if(collector.null_side.count(tables[i]) == 0 &&
       (probe_index == tables.size() || rows[i] > rows[probe_index])) {
      probe_index = i;
    }
  }
  if(probe_index == tables.size() || largest < min_rows) {
    return (warp_pj_no(__LINE__), none);
  }
  {
    warp_trx *trx = warp_get_trx(dynamic_cast<ha_warp *>(tables[0]->file)->ht, thd);
    if(trx != nullptr && (trx->isolation_level == ISO_SERIALIZABLE || trx->lock_in_share_mode)) {
      return (warp_pj_no(__LINE__), none);
    }
  }

  /* the tables by level: the probe table, then the others in the order they
     are reached from it (the small ones first on each depth) */
  const size_t n = tables.size();
  auto index_of = [&](TABLE *t) -> int {
    for(size_t i = 0; i < n; ++i) {
      if(tables[i] == t) return (int)i;
    }
    return -1;
  };
  struct join_edge {
    int a, b;
    std::vector<std::pair<Field *, Field *>> columns;  // a's column, b's column
    int null_index = -1;                               // the inner table of the LEFT JOIN, a or b
  };
  std::vector<join_edge> edges;
  for(const warp_pj_pair_info &pair : pairs) {
    const int a = index_of(pair.a->table);
    const int b = index_of(pair.b->table);
    if(a < 0 || b < 0 || !warp_pj_int_key(pair.a) || !warp_pj_int_key(pair.b)) {
      return (warp_pj_no(__LINE__), none);
    }
    const int null_index = pair.null_table != nullptr ? index_of(pair.null_table) : -1;
    if(pair.null_table != nullptr && a != null_index && b != null_index) {
      return (warp_pj_no(__LINE__), none);
    }
    bool found = false;
    for(auto &edge : edges) {
      if((edge.a == a && edge.b == b) || (edge.a == b && edge.b == a)) {
        const bool same = edge.a == a;
        Field *edge_a = same ? pair.a : pair.b;
        Field *edge_b = same ? pair.b : pair.a;
        /* a condition can be seen twice (the reference of a lookup and the
           condition of the join) */
        bool duplicate = false;
        for(const auto &column : edge.columns) {
          if(column.first == edge_a && column.second == edge_b) duplicate = true;
        }
        if(!duplicate) {
          edge.columns.push_back({edge_a, edge_b});
        }
        if(null_index >= 0) {
          edge.null_index = null_index;
        }
        found = true;
        break;
      }
    }
    if(!found) {
      join_edge edge;
      edge.a = a;
      edge.b = b;
      edge.columns.push_back({pair.a, pair.b});
      edge.null_index = null_index;
      edges.push_back(edge);
    }
  }

  std::vector<int> depth(n, -1), parent(n, -1), order;
  std::vector<int> parent_edge(n, -1);
  depth[probe_index] = 0;
  order.push_back((int)probe_index);
  for(size_t at = 0; at < order.size(); ++at) {
    const int current = order[at];
    std::vector<int> found;
    /* the tables of the inner side of a LEFT JOIN have no tables below them */
    if(collector.null_side.count(tables[current])) {
      continue;
    }
    for(size_t e = 0; e < edges.size(); ++e) {
      const int other = edges[e].a == current ? edges[e].b : (edges[e].b == current ? edges[e].a : -1);
      /* the inner table of a LEFT JOIN is reached from the other side only */
      if(other >= 0 && edges[e].null_index >= 0 && edges[e].null_index == current) {
        continue;
      }
      if(other >= 0 && depth[other] < 0) {
        depth[other] = depth[current] + 1;
        parent[other] = current;
        parent_edge[other] = (int)e;
        found.push_back(other);
      }
    }
    std::sort(found.begin(), found.end(), [&](int x, int y) { return rows[x] < rows[y]; });
    for(int f : found) order.push_back(f);
  }
  if(order.size() != n) {
    return (warp_pj_no(__LINE__), none);  // a table that is not joined by a column: a cross product
  }
  /* the small tables first among the ones of the same depth, the parents
     are always before their children */
  std::stable_sort(order.begin() + 1, order.end(), [&](int x, int y) {
    return depth[x] < depth[y];
  });

  /* build memory: the keys and images of the other tables */
  uint64_t bytes = 0;
  for(size_t i = 0; i < n; ++i) {
    if(i != probe_index) {
      bytes += rows[i] * 48;
    }
  }
  if(bytes > (6ULL << 30)) {
    return (warp_pj_no(__LINE__), none);
  }

  auto plan = std::make_shared<warp_pj_plan>();
  plan->thd = thd;
  plan->probe = tables[probe_index];
  plan->probe_handler = dynamic_cast<ha_warp *>(plan->probe->file);
  plan->nbuild = (int)n - 1;
  plan->tables.resize(n);
  plan->extras.resize(n);
  plan->tables[0].table = plan->probe;
  plan->tables[0].handler = plan->probe_handler;
  std::vector<int> level_of(n, -1);
  level_of[probe_index] = 0;
  for(size_t l = 1; l < n; ++l) {
    const int t = order[l];
    level_of[t] = (int)l;
    plan->tables[l].table = tables[t];
    plan->tables[l].handler = dynamic_cast<ha_warp *>(tables[t]->file);
    plan->tables[l].estimate = rows[t];
    plan->tables[l].null_side = collector.null_side.count(tables[t]) != 0;
    auto filters = collector.leaf_filters.find(tables[t]);
    if(filters != collector.leaf_filters.end()) {
      plan->tables[l].filters = filters->second;
    }
  }

  auto root_slot = [&](uint field) -> int {
    for(size_t i = 0; i < plan->root_cols.size(); ++i) {
      if(plan->root_cols[i] == field) return (int)i;
    }
    plan->root_cols.push_back(field);
    return (int)plan->root_cols.size() - 1;
  };
  auto value_slot = [&](int level, uint field) -> int {
    warp_pj_table &t = plan->tables[level];
    int s = t.slot_of(field);
    if(s < 0) {
      t.value_fields.push_back(field);
      s = (int)t.value_fields.size() - 1;
    }
    return s;
  };
  auto ref_of = [&](Field *field) -> warp_pj_ref {
    warp_pj_ref ref;
    ref.level = level_of[index_of(field->table)];
    ref.slot = ref.level == 0 ? root_slot(field->field_index()) : value_slot(ref.level, field->field_index());
    return ref;
  };

  /* the key of every table to its parent; the other conditions are checked
     when the second of their tables is reached */
  std::vector<bool> used_edge(edges.size(), false);
  for(size_t l = 1; l < n; ++l) {
    const int t = order[l];
    const int p = parent[t];
    const join_edge &edge = edges[parent_edge[t]];
    used_edge[parent_edge[t]] = true;
    if(edge.columns.size() > (size_t)WARP_PJ_MAX_KEY) {
      return (warp_pj_no(__LINE__), none);
    }
    warp_pj_table &table = plan->tables[l];
    table.edge.parent = level_of[p];
    table.edge.width = (int)edge.columns.size();
    for(size_t c = 0; c < edge.columns.size(); ++c) {
      Field *parent_field = edge.a == p ? edge.columns[c].first : edge.columns[c].second;
      Field *own_field = edge.a == p ? edge.columns[c].second : edge.columns[c].first;
      table.own_key_fields.push_back(own_field->field_index());
      value_slot((int)l, own_field->field_index());
      table.edge.slot[c] = table.edge.parent == 0 ? root_slot(parent_field->field_index())
                                                  : value_slot(table.edge.parent, parent_field->field_index());
    }
  }
  for(size_t e = 0; e < edges.size(); ++e) {
    if(used_edge[e]) continue;
    for(const auto &column : edges[e].columns) {
      warp_pj_extra extra;
      extra.a = ref_of(column.first);
      extra.b = ref_of(column.second);
      /* a condition of an outer join with a second table is not supported */
      if(plan->tables[extra.a.level].null_side || plan->tables[extra.b.level].null_side) {
        return (warp_pj_no(__LINE__), none);
      }
      plan->extras[std::max(extra.a.level, extra.b.level)].push_back(extra);
    }
  }

  /* what MySQL reads from the other tables, and the key columns: the fields
     must be in the read set, and be copyable */
  for(size_t l = 1; l < n; ++l) {
    warp_pj_table &t = plan->tables[l];
    TABLE *table = t.table;
    for(uint f : t.value_fields) {
      bitmap_set_bit(table->read_set, f);
    }
    t.ranges.push_back({0, (uint32_t)table->s->null_bytes});
    for(Field **field = table->field; *field; ++field) {
      if(!bitmap_is_set(table->read_set, (*field)->field_index())) continue;
      if(!warp_pj_copyable(*field)) {
        return (warp_pj_no(__LINE__), none);
      }
      const uint32_t offset = (uint32_t)((*field)->field_ptr() - table->record[0]);
      t.ranges.push_back({offset, (*field)->pack_length()});
    }
    uint32_t len = 0;
    for(const auto &r : t.ranges) len += r.second;
    t.image_len = len;
    t.vals.resize(t.value_fields.size());
    t.nulls.resize(t.value_fields.size());
  }
  /* the probe table is read through the normal scan of the rows that are
     returned */
  plan->probe_where = plan->probe_handler->push_where_clause;
  for(size_t i = 0; i + 1 < plan->probe_where.size(); ++i) {
    const std::string &w = plan->probe_where;
    if(w[i] == 'c' && (i == 0 || !isalnum((unsigned char)w[i - 1])) && isdigit((unsigned char)w[i + 1])) {
      uint f = 0;
      size_t j = i + 1;
      while(j < w.size() && isdigit((unsigned char)w[j])) {
        f = f * 10 + (w[j] - '0');
        ++j;
      }
      if(std::find(plan->where_fields.begin(), plan->where_fields.end(), f) == plan->where_fields.end()) {
        plan->where_fields.push_back(f);
      }
    }
  }
  /* NULL markers of the nullable columns of the probe table that are read */
  plan->root_null_slot.assign(plan->root_cols.size(), -1);
  for(size_t i = 0; i < plan->root_cols.size(); ++i) {
    if(plan->probe->field[plan->root_cols[i]]->is_nullable()) {
      plan->root_null_slot[i] = (int)plan->root_nulls.size();
      plan->root_nulls.push_back(plan->root_cols[i]);
    }
  }
  /* a condition about one build table of an inner join is evaluated when the
     table is read: the rows that do not pass it never join, so the probe rows
     that would be dropped later are not produced */
  for(auto it = residual->begin(); it != residual->end();) {
    bool moved = false;
    for(size_t l = 1; l < n && !moved; ++l) {
      warp_pj_table &t = plan->tables[l];
      if(!t.null_side && warp_pj_only_table(*it, t.table)) {
        t.filters.push_back(*it);
        moved = true;
      }
    }
    it = moved ? residual->erase(it) : it + 1;
  }
  /* the conditions of the tables that FastBit can evaluate are not evaluated by
     MySQL for every row that is read */
  for(size_t l = 1; l < n; ++l) {
    warp_pj_table &t = plan->tables[l];
    std::vector<Item *> remaining;
    for(Item *filter : t.filters) {
      std::string where;
      if(filter->type() == Item::FUNC_ITEM && t.handler->push_filter(filter, where)) {
        t.pushed_where += t.pushed_where.empty() ? "(" + where + ")" : " AND (" + where + ")";
      } else {
        remaining.push_back(filter);
      }
    }
    t.filters = remaining;
  }
  return plan;
}

/* Columns that an aggregation of the rows of the join (warp_par_agg.h) reads,
   added to the plan after it was made: a column of the probe table is read from
   every partition, a column of another table is kept with its values. */
static int warp_pj_add_root(warp_pj_plan &plan, uint field) {
  for(size_t i = 0; i < plan.root_cols.size(); ++i) {
    if(plan.root_cols[i] == field) return (int)i;
  }
  plan.root_cols.push_back(field);
  if(plan.probe->field[field]->is_nullable()) {
    plan.root_null_slot.push_back((int)plan.root_nulls.size());
    plan.root_nulls.push_back(field);
  } else {
    plan.root_null_slot.push_back(-1);
  }
  return (int)plan.root_cols.size() - 1;
}

static int warp_pj_add_value(warp_pj_plan &plan, int level, uint field) {
  warp_pj_table &t = plan.tables[level];
  int s = t.slot_of(field);
  if(s < 0) {
    t.value_fields.push_back(field);
    s = (int)t.value_fields.size() - 1;
    t.vals.resize(t.value_fields.size());
    t.nulls.resize(t.value_fields.size());
    bitmap_set_bit(t.table->read_set, field);
  }
  return s;
}

/* ---------------------------------------------------------------------- */
/* execution                                                              */
/* ---------------------------------------------------------------------- */

/* the rows of a table that pass its conditions -> images, key columns and hash */
static bool warp_pj_build(warp_pj_table &t, std::string *error) {
  TABLE *table = t.table;
  ha_warp *handler = t.handler;
  uchar *record = table->record[0];
  const size_t width = t.edge.width;
  std::vector<int64_t> keys;
  std::vector<int> own_slots;
  for(uint f : t.own_key_fields) {
    own_slots.push_back(t.slot_of(f));
  }
  t.arena.clear();
  t.rowids.clear();
  for(auto &v : t.vals) v.clear();
  for(auto &v : t.nulls) v.clear();
  t.nrows = 0;
  if(t.estimate > 0) {
    t.arena.reserve(std::min<uint64_t>(t.estimate, 1ULL << 26) * t.image_len);
  }

  handler->pj_plain_scan = true;
  const std::string saved_where = handler->push_where_clause;
  if(!t.pushed_where.empty()) {
    handler->push_where_clause = (saved_where.empty() || saved_where == "1=1")
                                     ? t.pushed_where
                                     : "(" + saved_where + ") AND (" + t.pushed_where + ")";
  }
  int rc = handler->ha_rnd_init(true);
  handler->push_where_clause = saved_where;
  handler->pj_plain_scan = false;
  if(rc != 0) {
    *error = "could not scan a table of the join";
    return false;
  }
  std::vector<int64_t> row_vals(t.value_fields.size());
  std::vector<uint8_t> row_nulls(t.value_fields.size());
  while((rc = handler->ha_rnd_next(record)) == 0) {
    bool joinable = true;
    for(size_t k = 0; k < t.value_fields.size(); ++k) {
      Field *field = table->field[t.value_fields[k]];
      if(field->is_null()) {
        row_nulls[k] = 1;
        row_vals[k] = 0;
      } else {
        row_nulls[k] = 0;
        row_vals[k] = field->val_int();
      }
    }
    for(int s : own_slots) {
      if(row_nulls[s]) joinable = false;  // NULL does not join
    }
    if(!joinable) {
      continue;
    }
    /* the conditions of the join about this table: a row that does not pass
       them does not match (and is not read by the joins above) */
    for(Item *filter : t.filters) {
      if(filter->val_int() == 0 || filter->null_value) {
        joinable = false;
        break;
      }
    }
    for(const auto &sf : t.semi) {
      Field *field = table->field[sf.first];
      if(field->is_null() || sf.second->count(field->val_int()) == 0) {
        joinable = false;
        break;
      }
    }
    if(!joinable) {
      continue;
    }
    for(size_t k = 0; k < row_vals.size(); ++k) {
      t.vals[k].push_back(row_vals[k]);
      t.nulls[k].push_back(row_nulls[k]);
    }
    for(int s : own_slots) {
      keys.push_back(row_vals[s]);
    }
    for(const auto &r : t.ranges) {
      t.arena.insert(t.arena.end(), record + r.first, record + r.first + r.second);
    }
    t.rowids.push_back(handler->pj_rowid());
    ++t.nrows;
    if(t.nrows >= 0xFFFFFFF0ULL) {
      handler->ha_rnd_end();
      *error = "a table of the join has too many rows";
      return false;
    }
  }
  handler->ha_rnd_end();
  if(rc != HA_ERR_END_OF_FILE) {
    *error = "could not read a table of the join";
    return false;
  }
  t.hash.build(keys, t.nrows, (int)width);
  return true;
}

/* expands one row of the probe table through the tables of the join */
struct warp_pj_prober {
  const warp_pj_plan &plan;
  const std::vector<warp_star_col> &cols;
  const std::vector<warp_star_col> &nulls;
  size_t row = 0;
  uint32_t ordinal = 0;
  uint32_t chosen[WARP_PJ_MAX_LEVELS];
  std::vector<uint32_t> *out = nullptr;
  warp_pj_sink *sink = nullptr;
  void *local = nullptr;

  inline bool value(const warp_pj_ref &ref, int64_t *v) const {
    if(ref.level == 0) {
      const int ns = plan.root_null_slot[ref.slot];
      if(ns >= 0 && nulls[ns].get(row) != 0) return false;
      *v = cols[ref.slot].get(row);
      return true;
    }
    const warp_pj_table &t = plan.tables[ref.level];
    const uint32_t r = chosen[ref.level];
    if(r == WARP_PJ_NULL_ROW || t.nulls[ref.slot][r]) return false;
    *v = t.vals[ref.slot][r];
    return true;
  }

  bool expand(int level) {
    if(level > plan.nbuild) {
      if(sink != nullptr) {
        sink->add(local, *this);
        return true;
      }
      out->push_back(ordinal);
      for(int l = 1; l <= plan.nbuild; ++l) {
        out->push_back(chosen[l]);
      }
      return true;
    }
    const warp_pj_table &t = plan.tables[level];
    int64_t key[WARP_PJ_MAX_KEY];
    for(int c = 0; c < t.edge.width; ++c) {
      warp_pj_ref ref;
      ref.level = t.edge.parent;
      ref.slot = t.edge.slot[c];
      if(!value(ref, &key[c])) {
        /* NULL does not join: the row of the table of a LEFT JOIN is a row of NULLs */
        if(t.null_side) {
          chosen[level] = WARP_PJ_NULL_ROW;
          return expand(level + 1);
        }
        return true;
      }
    }
    bool matched = false;
    for(uint32_t at = t.hash.head(key); at != 0; at = t.hash.next[at - 1]) {
      chosen[level] = at - 1;
      bool ok = true;
      for(const warp_pj_extra &extra : plan.extras[level]) {
        int64_t a, b;
        if(!value(extra.a, &a) || !value(extra.b, &b) || a != b) {
          ok = false;
          break;
        }
      }
      if(!ok) {
        continue;
      }
      matched = true;
      if(!expand(level + 1)) {
        return false;
      }
      if(out->size() > ((size_t)1 << 31)) {
        return false;  // the result is too large
      }
    }
    if(!matched && t.null_side) {
      chosen[level] = WARP_PJ_NULL_ROW;
      return expand(level + 1);
    }
    return true;
  }
};

/* the rows of one partition of the probe table that join */
static bool warp_pj_partition(warp_pj_plan &plan, ibis::part *part, const warp_star_snapshot &snap,
                              warp_pj_result *result, std::string *error) {
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
  if(plan.probe_where.empty() || plan.probe_where == "1=1") {
    everything.set(1, nrows);
    hits = &everything;
  } else {
    query.reset(new ibis::query((const char *)0, part, (const char *)0));
    query->addConditions(plan.probe_where.c_str());
    if(query->evaluate() < 0) {
      *error = std::string("could not evaluate the condition on ") + dir;
      plan.out_of_cache = true;
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
    plan.out_of_cache = true;
    return false;
  }

  std::vector<warp_star_col> cols(plan.root_cols.size());
  for(size_t k = 0; k < plan.root_cols.size(); ++k) {
    const std::string name = "c" + std::to_string(plan.root_cols[k]);
    if(!cols[k].load(part->getColumn(name.c_str()), nrows)) {
      *error = std::string("could not read column ") + name + " of " + dir;
      plan.out_of_cache = true;
      return false;
    }
  }
  std::vector<warp_star_col> nulls(plan.root_nulls.size());
  for(size_t k = 0; k < plan.root_nulls.size(); ++k) {
    const std::string name = "n" + std::to_string(plan.root_nulls[k]);
    if(!nulls[k].load(part->getColumn(name.c_str()), nrows)) {
      *error = std::string("could not read column ") + name + " of " + dir;
      plan.out_of_cache = true;
      return false;
    }
  }

  /* the rows that pass the conditions, in the order of the partition */
  std::vector<uint32_t> hit_rows;
  hit_rows.reserve(hits->cnt());
  for(ibis::bitvector::indexSet ix = hits->firstIndexSet(); ix.nIndices() > 0; ++ix) {
    const ibis::bitvector::word_t *indices = ix.indices();
    if(ix.isRange()) {
      for(ibis::bitvector::word_t row = indices[0]; row < indices[1]; ++row) {
        hit_rows.push_back(row);
      }
    } else {
      for(ibis::bitvector::word_t j = 0; j < ix.nIndices(); ++j) {
        hit_rows.push_back(indices[j]);
      }
    }
  }

  result->dir = dir;
  result->stride = 1 + (size_t)plan.nbuild;

  /* the rows are probed in slices, at the same time if the thread budget of
     the statement has threads to spare */
  const size_t min_slice = std::max<size_t>(THDVAR(plan.thd, parallel_min_rows), 1);
  size_t nslices = 1;
  if(hit_rows.size() >= 2 * min_slice) {
    nslices = std::min<size_t>(hit_rows.size() / min_slice, 64);
  }
  ibis::util::ThreadLease lease(nslices > 1 ? (unsigned)(nslices - 1) : 0);
  nslices = lease.granted() + 1;
  std::vector<std::vector<uint32_t>> slice_out(nslices);
  std::vector<std::exception_ptr> slice_error(nslices);
  std::atomic<bool> too_large(false);
  auto run_slice = [&](size_t k) {
    try {
      const size_t lo = hit_rows.size() * k / nslices;
      const size_t hi = hit_rows.size() * (k + 1) / nslices;
      warp_pj_prober prober{plan, cols, nulls};
      prober.out = &slice_out[k];
      /* the local state of the sink is ended when the slice is done */
      struct sink_scope {
        warp_pj_sink *sink;
        void *local;
        ~sink_scope() {
          if(sink != nullptr) sink->end(local);
        }
      } scope{plan.sink.get(), plan.sink ? plan.sink->begin() : nullptr};
      prober.sink = scope.sink;
      prober.local = scope.local;
      for(size_t i = lo; i < hi; ++i) {
        const size_t row = hit_rows[i];
        if(!invisible.empty() && invisible[row]) {
          continue;
        }
        prober.row = row;
        prober.ordinal = (uint32_t)i;
        if(!prober.expand(1)) {
          too_large = true;
          return;
        }
      }
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
  for(size_t k = started; k < nslices; ++k) {
    run_slice(k);
  }
  for(auto &t : threads) {
    t.join();
  }
  for(size_t k = 0; k < nslices; ++k) {
    if(slice_error[k]) {
      std::rethrow_exception(slice_error[k]);
    }
  }
  if(too_large) {
    *error = "the result of the join is too large";
    return false;
  }
  size_t total = 0;
  for(auto &v : slice_out) total += v.size();
  result->data.reserve(total);
  for(auto &v : slice_out) {
    result->data.insert(result->data.end(), v.begin(), v.end());
    std::vector<uint32_t>().swap(v);
  }
  if(plan.export_slot >= 0) {
    const int null_slot = plan.root_null_slot[plan.export_slot];
    result->keys.reserve(result->rows());
    for(size_t r = 0; r < result->rows(); ++r) {
      const size_t row = hit_rows[result->data[r * result->stride]];
      if(null_slot >= 0 && nulls[null_slot].get(row) != 0) {
        continue;
      }
      result->keys.push_back(cols[plan.export_slot].get(row));
    }
  }
  return true;
}

/* the values of the column of the derived table (it is materialized once, MySQL
   uses it as it is later) */
static bool warp_pj_read_semi(THD *thd, warp_pj_semi &semi, std::string *error) {
  semi.keys.clear();
  TABLE *derived = semi.inner->table;
  unique_ptr_destroy_only<RowIterator> iterator =
      CreateIteratorFromAccessPath(thd, semi.materialize, semi.join, false);
  if(!iterator || iterator->Init()) {
    *error = "could not materialize a derived table of the join";
    return false;
  }
  handler *file = derived->file;
  file->ha_index_or_rnd_end();
  if(file->ha_rnd_init(true) != 0) {
    *error = "could not read a derived table of the join";
    return false;
  }
  int rc;
  while((rc = file->ha_rnd_next(derived->record[0])) == 0) {
    if(!semi.inner->is_null()) {
      semi.keys.insert(semi.inner->val_int());
    }
  }
  file->ha_rnd_end();
  if(rc != HA_ERR_END_OF_FILE) {
    *error = "could not read a derived table of the join";
    return false;
  }
  return true;
}

static bool warp_pj_execute(warp_pj_plan &plan, std::string *error) {
  THD *thd = plan.thd;
  plan.serving = false;
  plan.results.clear();
  plan.close_output();
  plan.out_part = 0;
  plan.out_pos = 0;

  warp_star_snapshot snap;
  warp_trx *trx = warp_get_trx(plan.probe_handler->ht, thd);
  if(trx == nullptr) {
    *error = "no transaction";
    return false;
  }
  snap.trx_id = trx->trx_id;
  snap.repeatable_read = (trx->isolation_level == ISO_REPEATABLE_READ ||
                          trx->isolation_level == ISO_SERIALIZABLE);
  snap.check_rowids = warp_state->has_history_locks() || warp_state->delete_bitmap->may_have_bits();

  for(int l = 1; l <= plan.nbuild; ++l) plan.tables[l].semi.clear();
  for(warp_pj_semi &semi : plan.semis) {
    if(!warp_pj_read_semi(thd, semi, error)) {
      return false;
    }
    plan.tables[semi.level].semi.push_back({semi.outer->field_index(), &semi.keys});
  }

  /* the other tables first: what they hold decides which rows of the probe
     table are looked at */
  for(int l = 1; l <= plan.nbuild; ++l) {
    if(thd_killed(thd)) {
      *error = "interrupted";
      return false;
    }
    if(!warp_pj_build(plan.tables[l], error)) {
      return false;
    }
  }

  /* no row is written while the columns are read */
  warp_table_read_lock probe_lock(plan.probe_handler->get_warp_share()->data_dir_name);

  ibis::partList parts;
  ibis::util::gatherParts(parts, plan.probe_handler->get_warp_share()->data_dir_name, true);
  std::vector<ibis::part *> work;
  for(ibis::part *part : parts) {
    if(part != nullptr && part->nRows() > 0 &&
       std::string(part->currentDataDir()) !=
           std::string(plan.probe_handler->get_warp_share()->data_dir_name)) {
      work.push_back(part);
    }
  }

  size_t workers = THDVAR(thd, max_degree_of_parallelism);
  if(workers < 1) {
    workers = 1;
  }
  if(!work.empty()) {
    const uint64_t per_worker =
        std::max<uint64_t>(work[0]->nRows(), 1) * 8 * (plan.root_cols.size() + plan.root_nulls.size() + 3);
    workers = std::min<uint64_t>(
        workers, std::max<uint64_t>(1, (ibis::fileManager::currentCacheSize() / 2) / per_worker));
  }
  workers = std::min(workers, std::max<size_t>(work.size(), 1));

  plan.results.resize(work.size());
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
        if(k >= work.size() || failed.load()) {
          break;
        }
        if(thd_killed(thd)) {
          failed = true;
          break;
        }
        std::string message;
        if(!warp_pj_partition(plan, work[k], snap, &plan.results[k], &message)) {
          std::lock_guard<std::mutex> guard(error_mutex);
          if(first_error.empty()) {
            first_error = message;
          }
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
    (void)id;
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
  if(thd_killed(thd)) {
    *error = "interrupted";
    return false;
  }
  if(!first_error.empty()) {
    *error = first_error;
    return false;
  }
  /* partitions without a result row are not opened again */
  plan.results.erase(std::remove_if(plan.results.begin(), plan.results.end(),
                                    [](const warp_pj_result &r) { return r.rows() == 0; }),
                     plan.results.end());
  return true;
}

/* ---------------------------------------------------------------------- */
/* the handler                                                            */
/* ---------------------------------------------------------------------- */

int ha_warp::pj_run() {
  warp_pj_plan &plan = *par_join;
  /* the columns that MySQL reads from the probe table */
  set_column_set();
  if(plan.prefetched) {
    /* the join was made for the keys of a scan (pj_collect_keys) */
    plan.prefetched = false;
    plan.close_output();
    plan.out_part = 0;
    plan.out_pos = 0;
    plan.serving = true;
    return 0;
  }
  std::string error;
  try {
    if(!warp_pj_execute(plan, &error)) {
      if(thd_killed(plan.thd)) {
        plan.thd->send_kill_message();
        return HA_ERR_QUERY_INTERRUPTED;
      }
      if(plan.out_of_cache) {
        sql_print_error("WARP: the parallel hash join of %s failed: %s.  The FastBit cache "
                        "(warp_cache_size = %llu bytes) may be too small for this query.",
                        share->data_dir_name, error.c_str(), (unsigned long long)my_cache_size);
        return HA_ERR_OUT_OF_MEM;
      }
      sql_print_error("WARP: parallel hash join of %s failed: %s", share->data_dir_name, error.c_str());
      my_error(ER_UNKNOWN_ERROR, MYF(0));
      return HA_ERR_INTERNAL_ERROR;
    }
  } catch(const std::bad_alloc &) {
    sql_print_error("WARP: the parallel hash join of %s ran out of memory", share->data_dir_name);
    return HA_ERR_OUT_OF_MEM;
  } catch(const std::exception &e) {
    sql_print_error("WARP: parallel hash join of %s failed: %s", share->data_dir_name, e.what());
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return HA_ERR_INTERNAL_ERROR;
  }
  plan.computed = true;
  plan.serving = true;
  return 0;
}

/* The values of a column of the probe table in the rows of the join (the join
   is made now, the scan of the probe table uses the result later). */
bool ha_warp::pj_collect_keys(uint field, std::vector<int64_t> *keys) {
  warp_pj_plan &plan = *par_join;
  if(plan.export_ready && plan.export_slot >= 0 && plan.root_cols[plan.export_slot] == field) {
    *keys = plan.export_keys;
    return true;
  }
  if(plan.computed || plan.prefetched) {
    return false;   // made without the column: the join is not made again
  }
  int slot = -1;
  for(size_t i = 0; i < plan.root_cols.size(); ++i) {
    if(plan.root_cols[i] == field) slot = (int)i;
  }
  if(slot < 0) {
    return false;
  }
  plan.export_slot = slot;
  if(pj_run() != 0) {
    plan.export_slot = -1;
    return false;
  }
  plan.computed = true;
  plan.prefetched = true;
  plan.export_keys.clear();
  for(const warp_pj_result &r : plan.results) {
    plan.export_keys.insert(plan.export_keys.end(), r.keys.begin(), r.keys.end());
  }
  for(warp_pj_result &r : plan.results) {
    std::vector<int64_t>().swap(r.keys);
  }
  std::sort(plan.export_keys.begin(), plan.export_keys.end());
  plan.export_keys.erase(std::unique(plan.export_keys.begin(), plan.export_keys.end()), plan.export_keys.end());
  plan.export_ready = true;
  *keys = plan.export_keys;
  return true;
}

void ha_warp::pj_close() {
  if(par_join) {
    par_join->close_output();
  }
}

int ha_warp::pj_next(uchar *buf) {
  warp_pj_plan &plan = *par_join;
  for(;;) {
    if(plan.out_part >= plan.results.size()) {
      plan.close_output();
      return HA_ERR_END_OF_FILE;
    }
    if(thd_killed(plan.thd)) {
      return HA_ERR_QUERY_INTERRUPTED;
    }
    const warp_pj_result &result = plan.results[plan.out_part];
    if(plan.out_cursor == nullptr) {
      plan.out_base = ibis::table::create(result.dir.c_str());
      if(plan.out_base == nullptr) {
        return HA_ERR_OUT_OF_MEM;
      }
      std::string where = plan.probe_where.empty() ? "1=1" : plan.probe_where;
      plan.out_filtered = plan.out_base->select(column_set.c_str(), where.c_str());
      if(plan.out_filtered == nullptr) {
        plan.close_output();
        return HA_ERR_OUT_OF_MEM;
      }
      plan.out_cursor = plan.out_filtered->createCursor();
      register_cursor(plan.out_cursor, plan.out_filtered);
      plan.out_pos = 0;
    }
    if(plan.out_pos >= result.rows()) {
      plan.close_output();
      warp_release_partition(result.dir.c_str());
      ++plan.out_part;
      continue;
    }
    const uint32_t *tuple = &result.data[plan.out_pos * result.stride];
    ++plan.out_pos;
    if(plan.out_cursor->fetch(tuple[0]) != 0) {
      return HA_ERR_INTERNAL_ERROR;
    }
    const int rc = find_current_row(buf, plan.out_cursor);
    if(rc != 0) {
      return rc;
    }
    uint64_t rowid = 0;
    plan.out_cursor->getColumnAsULong("r", rowid);
    pj_set_rowid(rowid);
    /* The joins that were below this one called position() and the ones above
       copy the row id from the handler (ref): the join of the engine has to
       keep it up to date for every table. */
    position(buf);
    /* the tables that follow the probe table copy their rows when MySQL reads them */
    for(int l = 1; l <= plan.nbuild; ++l) {
      plan.current[l] = tuple[l];
    }
    return 0;
  }
}

bool ha_warp::pj_following() const {
  return pj_follower && pj_follower->serving;
}

int ha_warp::pj_follower_next(uchar *buf) {
  if(!pj_follower_pending) {
    return HA_ERR_END_OF_FILE;
  }
  pj_follower_pending = false;
  warp_pj_table &t = pj_follower->tables[pj_follower_level];
  const uint32_t row = pj_follower->current[pj_follower_level];
  if(row == WARP_PJ_NULL_ROW) {
    /* the inner table of a LEFT JOIN has no match: the join of MySQL (a left
       join with this scan as its inner side) makes the row of NULLs */
    return HA_ERR_END_OF_FILE;
  }
  pj_set_rowid(t.rowids[row]);
  position(buf);  // see pj_next
  const uchar *image = &t.arena[(size_t)row * t.image_len];
  for(const auto &r : t.ranges) {
    memcpy(buf + r.first, image, r.second);
    image += r.second;
  }
  return 0;
}

/* The access path `path` (a tree of joins) is replaced by a scan of the probe
   table, with a FILTER for the conditions that MySQL has to evaluate. */
static bool warp_pj_replace(THD *thd, AccessPath *path, JOIN *join, const std::vector<warp_pj_semi> &semis) {
  std::vector<Item *> residual;
  std::shared_ptr<warp_pj_plan> plan = warp_pj_make_plan(thd, path, &residual);
  if(!plan) {
    return false;
  }
  /* the tables are joined by the engine now, not by the hash joins that the keys
     were meant for (and the scans of the engine's join read the tables) */
  plan->probe_handler->semi_in.reset();
  for(int l = 1; l <= plan->nbuild; ++l) {
    plan->tables[l].handler->semi_in.reset();
  }
  for(const warp_pj_semi &candidate : semis) {
    for(int l = 1; l <= plan->nbuild; ++l) {
      if(plan->tables[l].table == candidate.outer->table && !plan->tables[l].null_side) {
        plan->semis.push_back(candidate);
        plan->semis.back().level = l;
        break;
      }
    }
  }
  AccessPath *scan = NewTableScanAccessPath(thd, plan->probe, false);
  scan->set_num_output_rows(path->num_output_rows());
  scan->set_cost(path->cost());
  AccessPath *top = scan;
  /* the other tables of the join stay in the plan, joined to the probe table
     by a nested loop whose inner side returns the row that the engine chose */
  for(int l = 1; l <= plan->nbuild; ++l) {
    AccessPath *follower = NewTableScanAccessPath(thd, plan->tables[l].table, false);
    follower->set_num_output_rows(1.0);
    follower->set_cost(0.0);
    AccessPath *nested = new (thd->mem_root) AccessPath;
    nested->type = AccessPath::NESTED_LOOP_JOIN;
    nested->nested_loop_join().outer = top;
    nested->nested_loop_join().inner = follower;
    nested->nested_loop_join().join_type =
        plan->tables[l].null_side ? JoinType::OUTER : JoinType::INNER;
    nested->nested_loop_join().pfs_batch_mode = false;
    nested->nested_loop_join().already_expanded_predicates = false;
    nested->nested_loop_join().join_predicate = nullptr;
    nested->set_num_output_rows(path->num_output_rows());
    nested->set_cost(path->cost());
    top = nested;
    plan->tables[l].handler->pj_follower = plan;
    plan->tables[l].handler->pj_follower_level = l;
    plan->tables[l].handler->pj_follower_pending = false;
  }
  for(Item *condition : residual) {
    top = NewFilterAccessPath(thd, top, condition);
    top->set_num_output_rows(path->num_output_rows());
    top->set_cost(path->cost());
  }
  *path = *top;
  plan->probe_handler->par_join = plan;
  return true;
}

/* the inner side of a nested loop is a derived table that is read by an index
   on one column, with a column of another table as the key */
static void warp_pj_find_semi(AccessPath *inner, JOIN *join, std::vector<warp_pj_semi> *semis) {
  if(inner->type != AccessPath::MATERIALIZE || inner->materialize().param == nullptr ||
     inner->materialize().param->rematerialize) {
    return;
  }
  AccessPath *table_path = inner->materialize().table_path;
  if(table_path == nullptr || (table_path->type != AccessPath::REF && table_path->type != AccessPath::EQ_REF)) {
    return;
  }
  const bool is_ref = table_path->type == AccessPath::REF;
  TABLE *derived = is_ref ? table_path->ref().table : table_path->eq_ref().table;
  Index_lookup *ref = is_ref ? table_path->ref().ref : table_path->eq_ref().ref;
  if(derived == nullptr || ref == nullptr || ref->key_parts != 1) {
    return;
  }
  Item *value = ref->items[0]->real_item();
  if(value->type() != Item::FIELD_ITEM) {
    return;
  }
  Field *outer = down_cast<Item_field *>(value)->field;
  Field *key = derived->key_info[ref->key].key_part[0].field;
  if(outer == nullptr || key == nullptr || outer->table == derived || !warp_pj_int_key(outer) ||
     !warp_pj_int_key(key) || dynamic_cast<ha_warp *>(outer->table->file) == nullptr) {
    return;
  }
  warp_pj_semi semi;
  semi.materialize = inner;
  semi.join = join;
  semi.outer = outer;
  semi.inner = key;
  semis->push_back(semi);
}

/* Called from push_to_engine: the joins that the engine can evaluate are
   replaced. */
static void warp_try_parallel_join(THD *thd, AccessPath *root, JOIN *join) {
  if(!THDVAR(thd, parallel_hash_join) ||
     (thd->lex->sql_command != SQLCOM_SELECT && thd->lex->sql_command != SQLCOM_CREATE_TABLE &&
      thd->lex->sql_command != SQLCOM_INSERT_SELECT)) {
    return;
  }
  /* a block that is executed again and again would build its hash tables
     every time */
  if(warp_block_is_executed_again(join)) {
    warp_pj_no(__LINE__);
    return;
  }
  std::vector<warp_pj_semi> semis;
  WalkAccessPaths(root, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [&](AccessPath *path, const JOIN *) {
                    if(path->type == AccessPath::NESTED_LOOP_JOIN &&
                       path->nested_loop_join().join_type == JoinType::INNER) {
                      warp_pj_find_semi(path->nested_loop_join().inner, join, &semis);
                    }
                    if(path->type == AccessPath::HASH_JOIN ||
                       path->type == AccessPath::NESTED_LOOP_JOIN) {
                      if(warp_pj_replace(thd, path, join, semis)) {
                        return true;  // everything below it is the engine's now
                      }
                    }
                    return false;
                  });
}

#endif
