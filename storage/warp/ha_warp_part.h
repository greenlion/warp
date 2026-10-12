/* Copyright (c) 2026 Justin Swanhart

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

/* Partitioned WARP tables.

   Every partition (every subpartition of a subpartitioned table) is stored
   like a WARP table of its own, in the data directory of the partition:

     ./test/t#p#p0.data           partition p0 of test.t
     ./test/t#p#p0#sp#s0.data     subpartition s0 of p0

   ha_warp_part opens an ha_warp for every partition, on the same TABLE, and
   Partition_helper sends each row to its partition and reads the
   partitions that the statement uses (partition pruning).  The row ids and
   the transactions of WARP are global, so the partitions need nothing of
   their own for them.  A condition pushed down to the table is pushed down
   to every partition.

   ALTER TABLE ... DROP PARTITION removes the directory of the partition,
   ADD PARTITION (RANGE and LIST) creates an empty one and TRUNCATE
   PARTITION empties the partition: the other partitions are not read or
   written.  The other changes of the partitioning are made by copying the
   table (ALGORITHM=COPY). */

#ifndef HA_WARP_PART_INCLUDED
#define HA_WARP_PART_INCLUDED

#include <memory>
#include <string>
#include <vector>

#include "ha_warp.h"
#include "sql/partition_info.h"
#include "sql/partitioning/partition_handler.h"

class ha_warp_part : public handler,
                     public Partition_helper,
                     public Partition_handler {
 public:
  ha_warp_part(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_warp_part() override;

  /* The handler of the first partition, which makes the FastBit condition of
     a pushed down condition (the partitions have the same columns), see
     warp_push_table_conditions */
  ha_warp *condition_handler();
  /* The condition that every partition evaluates */
  void set_pushed_condition(const std::string &where);

  const char *table_type() const override { return "WARP"; }
  Table_flags table_flags() const override;
  ulong index_flags(uint, uint, bool) const override {
    return HA_READ_NEXT | HA_READ_RANGE;
  }
  uint max_supported_record_length() const override {
    return HA_MAX_REC_LENGTH;
  }
  uint max_supported_keys() const override { return 64; }
  uint max_supported_key_parts() const override { return 16; }
  uint max_supported_key_length() const override { return 3072; }
  uint max_supported_key_part_length(HA_CREATE_INFO *) const override {
    return 3072;
  }
  double scan_time() override;
  ha_rows estimate_rows_upper_bound() override { return HA_POS_ERROR; }
  const handlerton *hton_supporting_engine_pushdown() override { return ht; }
  std::string explain_extra() const override;

  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;
  int close() override;
  void change_table_ptr(TABLE *table_arg, TABLE_SHARE *share) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;
  int delete_table(const char *name, const dd::Table *table_def) override;
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;
  int external_lock(THD *thd, int lock_type) override;
  int start_stmt(THD *thd, thr_lock_type lock_type) override;
  void start_bulk_insert(ha_rows rows) override;
  int end_bulk_insert() override;
  int extra(enum ha_extra_function operation) override;
  int reset() override;

  int write_row(uchar *buf) override { return ph_write_row(buf); }
  int update_row(const uchar *old_data, uchar *new_data) override {
    return ph_update_row(old_data, new_data);
  }
  int delete_row(const uchar *buf) override { return ph_delete_row(buf); }
  int delete_all_rows() override;

  int rnd_init(bool scan) override { return ph_rnd_init(scan); }
  int rnd_next(uchar *buf) override { return ph_rnd_next(buf); }
  int rnd_end() override { return ph_rnd_end(); }
  void position(const uchar *record) override { ph_position(record); }
  int rnd_pos(uchar *buf, uchar *pos) override;

  int index_init(uint idx, bool sorted) override;
  int index_end() override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override {
    return ph_index_read_map(buf, key, keypart_map, find_flag);
  }
  int index_read_idx_map(uchar *buf, uint index, const uchar *key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override {
    return ph_index_read_idx_map(buf, index, key, keypart_map, find_flag);
  }
  int index_next(uchar *buf) override { return ph_index_next(buf); }
  int index_next_same(uchar *buf, const uchar *, uint keylen) override {
    return ph_index_next_same(buf, keylen);
  }
  int index_first(uchar *buf) override { return ph_index_first(buf); }
  int read_range_first(const key_range *start_key, const key_range *end_key,
                       bool eq_range_arg, bool sorted) override {
    return ph_read_range_first(start_key, end_key, eq_range_arg, sorted);
  }
  int read_range_next() override { return ph_read_range_next(); }
  ha_rows records_in_range(uint idx, key_range *min_key,
                           key_range *max_key) override;

  int info(uint flag) override;
  int records(ha_rows *num_rows) override;
  int check(THD *thd, HA_CHECK_OPT *check_opt) override;
  int repair(THD *thd, HA_CHECK_OPT *check_opt) override;
  int optimize(THD *thd, HA_CHECK_OPT *check_opt) override;
  void print_error(int error, myf errflag) override;

  enum_alter_inplace_result check_if_supported_inplace_alter(
      TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;
  bool inplace_alter_table(TABLE *altered_table,
                           Alter_inplace_info *ha_alter_info,
                           const dd::Table *old_table_def,
                           dd::Table *new_table_def) override;
  bool commit_inplace_alter_table(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info,
                                  bool commit,
                                  const dd::Table *old_table_def,
                                  dd::Table *new_table_def) override;

  /* AUTO_INCREMENT: the next value of the table is the largest next value
     of the partitions, Partition_helper gives the values out */
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values, ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  void release_auto_increment() override { ph_release_auto_increment(); }
  void update_create_info(HA_CREATE_INFO *create_info) override {
    if (!(create_info->used_fields & HA_CREATE_USED_AUTO) &&
        table->found_next_number_field != nullptr) {
      info(HA_STATUS_AUTO);
      create_info->auto_increment_value = stats.auto_increment_value;
    }
  }

  uint32_t calculate_key_hash_value(Field **field_array) override {
    return ph_calculate_key_hash_value(field_array);
  }

  /* Partition_handler */
  Partition_handler *get_partition_handler() override { return this; }
  handler *get_handler() override { return this; }
  void get_dynamic_partition_info(ha_statistics *stat_info,
                                  ha_checksum *check_sum,
                                  uint part_id) override {
    get_dynamic_partition_info_low(stat_info, check_sum, part_id);
  }
  void set_part_info(partition_info *part_info, bool early) override {
    set_part_info_low(part_info, early);
  }
  uint alter_flags(uint flags) const override;
  enum row_type get_partition_row_type(const dd::Table *, uint) override {
    return ROW_TYPE_NOT_USED;
  }

 private:
  int truncate_partition_low(dd::Table *table_def) override;

  /* Partition_helper */
  THD *get_thd() const override { return ha_thd(); }
  TABLE *get_table() const override { return table; }
  bool get_eq_range() const override { return eq_range; }
  void set_eq_range(bool eq_range_arg) override { eq_range = eq_range_arg; }
  void set_range_key_part(KEY_PART_INFO *key_part) override {
    range_key_part = key_part;
  }
  int write_row_in_part(uint part_id, uchar *buf) override;
  int update_row_in_part(uint part_id, const uchar *old_data,
                         uchar *new_data) override;
  int delete_row_in_part(uint part_id, const uchar *buf) override;
  int initialize_auto_increment(bool no_lock) override;
  int rnd_init_in_part(uint part_id, bool table_scan) override;
  int rnd_next_in_part(uint part_id, uchar *buf) override;
  int rnd_end_in_part(uint part_id, bool scan) override;
  void position_in_last_part(uchar *ref_arg, const uchar *row) override;
  int index_first_in_part(uint part, uchar *buf) override;
  int index_last_in_part(uint, uchar *) override {
    return HA_ERR_WRONG_COMMAND;
  }
  int index_prev_in_part(uint, uchar *) override {
    return HA_ERR_WRONG_COMMAND;
  }
  int index_next_in_part(uint part, uchar *buf) override;
  int index_next_same_in_part(uint part, uchar *buf, const uchar *key,
                              uint length) override;
  int index_read_map_in_part(uint part, uchar *buf, const uchar *key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag) override;
  int index_read_last_map_in_part(uint, uchar *, const uchar *,
                                  key_part_map) override {
    return HA_ERR_WRONG_COMMAND;
  }
  int read_range_first_in_part(uint part, uchar *buf,
                               const key_range *start_key,
                               const key_range *end_key, bool sorted) override;
  int read_range_next_in_part(uint part, uchar *buf) override;
  int index_read_idx_map_in_part(uint part, uchar *buf, uint index,
                                 const uchar *key, key_part_map keypart_map,
                                 enum ha_rkey_function find_flag) override;
  int write_row_in_new_part(uint) override { return HA_ERR_WRONG_COMMAND; }

  /* The path of every partition of part_info, in the order of the partition
     ids (the subpartitions of a partition one after the other) */
  static std::vector<std::string> partition_paths(const char *table_path,
                                                  partition_info *part_info);
  /* The data directories of the partitions of a table that are on disk */
  static std::vector<std::string> partitions_on_disk(const char *table_path);

  /* the partitions, by partition id */
  std::vector<std::unique_ptr<ha_warp>> m_parts;
  std::vector<std::string> m_part_paths;
  /* the partitions whose index is initialized (index_init) */
  std::vector<bool> m_index_inited;
  /* the partitions of the table were closed by an ALTER TABLE that changed
     the partitions (commit_inplace_alter_table) */
  bool m_parts_closed = false;
  Partition_share *m_part_share = nullptr;
};

#endif
