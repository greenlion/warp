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

/* Partitioned WARP tables, see ha_warp_part.h */

#include "ha_warp_part.h"

#include <dirent.h>
#include <string.h>

#include "my_byteorder.h"
#include "mysql/strings/m_ctype.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/table.h"
#include "sql/handler.h"
#include "sql/partition_element.h"
#include "sql/sql_alter.h"
#include "sql/sql_class.h"
#include "sql/sql_partition.h"
#include "sql/sql_table.h"

/* The path of a partition: the path of the table followed by #p# and the
   name of the partition (#sp# and the name of the subpartition), like
   InnoDB names them.  The names are compared without case, so they are
   written in lower case, encoded like the names of tables in file names. */
static std::string warp_partition_path(const char *table_path,
                                       const char *part_name,
                                       const char *subpart_name) {
  auto encode = [](const char *name) {
    char lower[NAME_LEN + 1];
    snprintf(lower, sizeof(lower), "%s", name);
    my_casedn_str(system_charset_info, lower);
    char file_name[FN_REFLEN];
    tablename_to_filename(lower, file_name, sizeof(file_name));
    return std::string(file_name);
  };
  std::string path = std::string(table_path) + "#p#" + encode(part_name);
  if (subpart_name != nullptr) {
    path += "#sp#" + encode(subpart_name);
  }
  return path;
}

std::vector<std::string> ha_warp_part::partition_paths(
    const char *table_path, partition_info *part_info) {
  std::vector<std::string> paths;
  List_iterator<partition_element> part_it(part_info->partitions);
  partition_element *part_elem;
  while ((part_elem = part_it++) != nullptr) {
    if (part_info->is_sub_partitioned()) {
      List_iterator<partition_element> sub_it(part_elem->subpartitions);
      partition_element *sub_elem;
      while ((sub_elem = sub_it++) != nullptr) {
        paths.push_back(warp_partition_path(table_path,
                                            part_elem->partition_name,
                                            sub_elem->partition_name));
      }
    } else {
      paths.push_back(
          warp_partition_path(table_path, part_elem->partition_name, nullptr));
    }
  }
  return paths;
}

/* The partitions of a table that are on disk, found by their data
   directories (<table>#p#....data).  The '#' of a table name is encoded in
   its file name, so the directories of another table can not match. */
std::vector<std::string> ha_warp_part::partitions_on_disk(
    const char *table_path) {
  std::vector<std::string> paths;
  const std::string path = table_path;
  const size_t slash = path.rfind('/');
  const std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  const std::string prefix =
      (slash == std::string::npos ? path : path.substr(slash + 1)) + "#p#";
  DIR *d = opendir(dir.c_str());
  if (d == nullptr) {
    return paths;
  }
  struct dirent *entry;
  while ((entry = readdir(d)) != nullptr) {
    const std::string name = entry->d_name;
    if (name.size() > prefix.size() + 5 &&
        name.compare(0, prefix.size(), prefix) == 0 &&
        name.compare(name.size() - 5, 5, ".data") == 0) {
      paths.push_back(dir + "/" + name.substr(0, name.size() - 5));
    }
  }
  closedir(d);
  return paths;
}

ha_warp_part::ha_warp_part(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg), Partition_helper(this) {}

ha_warp_part::~ha_warp_part() {}

handler::Table_flags ha_warp_part::table_flags() const {
  return warp_table_flags();
}

ha_warp *ha_warp_part::condition_handler() {
  if (m_parts.empty() || m_parts_closed) {
    return nullptr;
  }
  return m_parts[0].get();
}

void ha_warp_part::set_pushed_condition(const std::string &where) {
  for (auto &part : m_parts) {
    part->push_where_clause = where;
    part->star_agg.reset();
  }
}

double ha_warp_part::scan_time() {
  return 1.0 / (stats.records > 0 ? stats.records : 1);
}

std::string ha_warp_part::explain_extra() const {
  if (m_parts.empty() || m_parts_closed) {
    return "";
  }
  return m_parts[0]->explain_extra();
}

int ha_warp_part::open(const char *name, int mode, uint test_if_locked,
                       const dd::Table *table_def) {
  DBUG_TRACE;
  if (m_part_info == nullptr) {
    /* ::clone() */
    set_part_info_low(table->part_info, false);
  }
  /* The partition information that is shared by the handlers of the table */
  lock_shared_ha_data();
  m_part_share = static_cast<Partition_share *>(get_ha_share_ptr());
  if (m_part_share == nullptr) {
    m_part_share = new (std::nothrow) Partition_share();
    if (m_part_share == nullptr) {
      unlock_shared_ha_data();
      return HA_ERR_OUT_OF_MEM;
    }
    set_ha_share_ptr(static_cast<Handler_share *>(m_part_share));
  }
  if (m_part_share->partition_name_hash == nullptr &&
      m_part_share->populate_partition_name_hash(m_part_info)) {
    unlock_shared_ha_data();
    return HA_ERR_INTERNAL_ERROR;
  }
  unlock_shared_ha_data();

  m_part_paths = partition_paths(name, m_part_info);
  m_parts.clear();
  for (size_t i = 0; i < m_part_paths.size(); ++i) {
    std::unique_ptr<ha_warp> part(new (std::nothrow) ha_warp(ht, table_share));
    if (part == nullptr) {
      close();
      return HA_ERR_OUT_OF_MEM;
    }
    part->is_partition_child = true;
    part->pd_alias_suffix = "#p" + std::to_string(i);
    const int rc = part->ha_open(table, m_part_paths[i].c_str(), mode,
                                 test_if_locked, table_def);
    if (rc != 0) {
      close();
      return rc;
    }
    m_parts.push_back(std::move(part));
  }
  m_index_inited.assign(m_parts.size(), false);
  m_parts_closed = false;

  if (open_partitioning(m_part_share)) {
    close();
    return HA_ERR_INITIALIZATION;
  }
  /* the partition id, then the row id in the partition */
  ref_length = PARTITION_BYTES_IN_POS +
               (m_parts.empty() ? sizeof(my_off_t) : m_parts[0]->ref_length);
  info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
  return 0;
}

int ha_warp_part::close() {
  DBUG_TRACE;
  int rc = 0;
  if (!m_parts_closed) {
    for (auto &part : m_parts) {
      const int part_rc = part->ha_close();
      if (part_rc != 0 && rc == 0) rc = part_rc;
    }
  }
  m_parts.clear();
  m_parts_closed = false;
  close_partitioning();
  return rc;
}

void ha_warp_part::change_table_ptr(TABLE *table_arg, TABLE_SHARE *share) {
  handler::change_table_ptr(table_arg, share);
  for (auto &part : m_parts) {
    part->change_table_ptr(table_arg, share);
  }
}

int ha_warp_part::create(const char *name, TABLE *form,
                         HA_CREATE_INFO *create_info, dd::Table *) {
  DBUG_TRACE;
  if (form->part_info == nullptr) {
    return HA_ERR_INTERNAL_ERROR;
  }
  /* the partitions of a table that was dropped without its files */
  for (const std::string &path : partitions_on_disk(name)) {
    ha_warp old_part(ht, form->s);
    old_part.delete_table(path.c_str(), nullptr);
  }
  for (const std::string &path : partition_paths(name, form->part_info)) {
    ha_warp part(ht, form->s);
    const int rc = part.create(path.c_str(), form, create_info, nullptr);
    if (rc != 0) {
      delete_table(name, nullptr);
      return rc;
    }
  }
  return 0;
}

int ha_warp_part::delete_table(const char *name, const dd::Table *) {
  DBUG_TRACE;
  /* every partition that is on disk, including those of an ALTER TABLE that
     did not finish */
  int rc = 0;
  for (const std::string &path : partitions_on_disk(name)) {
    ha_warp part(ht, table_share);
    const int part_rc = part.delete_table(path.c_str(), nullptr);
    if (part_rc != 0 && rc == 0) rc = part_rc;
  }
  return rc;
}

int ha_warp_part::rename_table(const char *from, const char *to,
                               const dd::Table *, dd::Table *) {
  DBUG_TRACE;
  const size_t from_len = strlen(from);
  int rc = 0;
  for (const std::string &path : partitions_on_disk(from)) {
    /* the name of the partition after the name of the table */
    const std::string to_path = std::string(to) + path.substr(from_len);
    ha_warp part(ht, table_share);
    const int part_rc = part.rename_table(path.c_str(), to_path.c_str(),
                                          nullptr, nullptr);
    if (part_rc != 0 && rc == 0) rc = part_rc;
  }
  return rc;
}

THR_LOCK_DATA **ha_warp_part::store_lock(THD *thd, THR_LOCK_DATA **to,
                                         enum thr_lock_type lock_type) {
  /* WARP takes no table locks of its own, the partitions note the kind of
     lock (FOR UPDATE, LOCK IN SHARE MODE) */
  for (auto &part : m_parts) {
    to = part->store_lock(thd, to, lock_type);
  }
  return to;
}

int ha_warp_part::external_lock(THD *thd, int lock_type) {
  if (m_parts_closed) {
    return 0;
  }
  int rc = 0;
  for (auto &part : m_parts) {
    const int part_rc = part->ha_external_lock(thd, lock_type);
    if (part_rc != 0 && rc == 0) rc = part_rc;
  }
  return rc;
}

int ha_warp_part::start_stmt(THD *thd, thr_lock_type lock_type) {
  int rc = 0;
  for (auto &part : m_parts) {
    const int part_rc = part->start_stmt(thd, lock_type);
    if (part_rc != 0 && rc == 0) rc = part_rc;
  }
  return rc;
}

void ha_warp_part::start_bulk_insert(ha_rows rows) {
  for (auto &part : m_parts) {
    part->ha_start_bulk_insert(rows);
  }
}

int ha_warp_part::end_bulk_insert() {
  /* the buffered rows of every partition are written */
  int rc = 0;
  for (auto &part : m_parts) {
    const int part_rc = part->ha_end_bulk_insert();
    if (part_rc != 0 && rc == 0) rc = part_rc;
  }
  return rc;
}

int ha_warp_part::extra(enum ha_extra_function operation) {
  if (m_parts_closed) {
    return 0;
  }
  for (auto &part : m_parts) {
    part->extra(operation);
  }
  return 0;
}

int ha_warp_part::reset() {
  int rc = 0;
  if (!m_parts_closed) {
    for (auto &part : m_parts) {
      const int part_rc = part->ha_reset();
      if (part_rc != 0 && rc == 0) rc = part_rc;
    }
  }
  return rc;
}

int ha_warp_part::delete_all_rows() {
  int rc = 0;
  for (auto &part : m_parts) {
    const int part_rc = part->delete_all_rows();
    if (part_rc != 0 && rc == 0) rc = part_rc;
    part->update_row_count();
  }
  return rc;
}

int ha_warp_part::truncate_partition_low(dd::Table *) {
  /* the partitions of read_partitions are emptied: their data directories
     are made again */
  int rc = 0;
  for (uint i = m_part_info->get_first_used_partition(); i < MY_BIT_NONE;
       i = m_part_info->get_next_used_partition(i)) {
    const int part_rc = m_parts[i]->delete_all_rows();
    if (part_rc != 0 && rc == 0) rc = part_rc;
    m_parts[i]->update_row_count();
  }
  return rc;
}

/* Row operations in a partition */

int ha_warp_part::write_row_in_part(uint part_id, uchar *buf) {
  m_last_part = part_id;
  return m_parts[part_id]->write_row(buf);
}

int ha_warp_part::update_row_in_part(uint part_id, const uchar *old_data,
                                     uchar *new_data) {
  m_last_part = part_id;
  return m_parts[part_id]->update_row(old_data, new_data);
}

int ha_warp_part::delete_row_in_part(uint part_id, const uchar *buf) {
  m_last_part = part_id;
  return m_parts[part_id]->delete_row(buf);
}

int ha_warp_part::rnd_init_in_part(uint part_id, bool table_scan) {
  return m_parts[part_id]->ha_rnd_init(table_scan);
}

int ha_warp_part::rnd_next_in_part(uint part_id, uchar *buf) {
  m_last_part = part_id;
  return m_parts[part_id]->rnd_next(buf);
}

int ha_warp_part::rnd_end_in_part(uint part_id, bool) {
  return m_parts[part_id]->ha_rnd_end();
}

void ha_warp_part::position_in_last_part(uchar *ref_arg, const uchar *row) {
  ha_warp *part = m_parts[m_last_part].get();
  part->position(row);
  memcpy(ref_arg, part->ref, part->ref_length);
}

int ha_warp_part::rnd_pos(uchar *buf, uchar *pos) {
  const uint part_id = uint2korr(pos);
  if (part_id >= m_parts.size()) {
    return HA_ERR_KEY_NOT_FOUND;
  }
  m_last_part = part_id;
  return m_parts[part_id]->rnd_pos(buf, pos + PARTITION_BYTES_IN_POS);
}

/* Index reads.  An index of WARP is a condition on its columns (see
   ha_warp::make_key_condition), the rows of a partition are not returned in
   the order of the key and the partitions are read one after the other. */

int ha_warp_part::index_init(uint idx, bool) {
  /* the rows are not in the order of the index, there is nothing to merge */
  const int rc = ph_index_init_setup(idx, false);
  if (rc != 0) {
    return rc;
  }
  active_index = idx;
  for (uint i = m_part_info->get_first_used_partition(); i < MY_BIT_NONE;
       i = m_part_info->get_next_used_partition(i)) {
    const int part_rc = m_parts[i]->ha_index_init(idx, false);
    if (part_rc != 0) {
      index_end();
      return part_rc;
    }
    m_index_inited[i] = true;
  }
  return 0;
}

int ha_warp_part::index_end() {
  int rc = 0;
  for (size_t i = 0; i < m_parts.size(); ++i) {
    if (m_index_inited[i]) {
      const int part_rc = m_parts[i]->ha_index_end();
      if (part_rc != 0 && rc == 0) rc = part_rc;
      m_index_inited[i] = false;
    }
  }
  active_index = MAX_KEY;
  return rc;
}

int ha_warp_part::index_first_in_part(uint part, uchar *buf) {
  m_last_part = part;
  return m_parts[part]->index_first(buf);
}

int ha_warp_part::index_next_in_part(uint part, uchar *buf) {
  m_last_part = part;
  return m_parts[part]->index_next(buf);
}

int ha_warp_part::index_next_same_in_part(uint part, uchar *buf,
                                          const uchar *key, uint length) {
  m_last_part = part;
  return m_parts[part]->index_next_same(buf, key, length);
}

int ha_warp_part::index_read_map_in_part(uint part, uchar *buf,
                                         const uchar *key,
                                         key_part_map keypart_map,
                                         enum ha_rkey_function find_flag) {
  m_last_part = part;
  return m_parts[part]->index_read_map(buf, key, keypart_map, find_flag);
}

int ha_warp_part::read_range_first_in_part(uint part, uchar *buf,
                                           const key_range *start_key,
                                           const key_range *end_key,
                                           bool sorted) {
  m_last_part = part;
  ha_warp *p = m_parts[part].get();
  const int rc =
      p->read_range_first(start_key, end_key, get_eq_range(), sorted);
  if (rc == 0 && buf != nullptr && buf != table->record[0]) {
    memcpy(buf, table->record[0], m_rec_length);
  }
  return rc;
}

int ha_warp_part::read_range_next_in_part(uint part, uchar *buf) {
  m_last_part = part;
  const int rc = m_parts[part]->read_range_next();
  if (rc == 0 && buf != nullptr && buf != table->record[0]) {
    memcpy(buf, table->record[0], m_rec_length);
  }
  return rc;
}

int ha_warp_part::index_read_idx_map_in_part(uint part, uchar *buf,
                                             uint index, const uchar *key,
                                             key_part_map keypart_map,
                                             enum ha_rkey_function find_flag) {
  m_last_part = part;
  return m_parts[part]->index_read_idx_map(buf, index, key, keypart_map,
                                           find_flag);
}

ha_rows ha_warp_part::records_in_range(uint idx, key_range *min_key,
                                       key_range *max_key) {
  ha_rows rows = 0;
  for (uint i = m_part_info->get_first_used_partition(); i < MY_BIT_NONE;
       i = m_part_info->get_next_used_partition(i)) {
    const ha_rows part_rows =
        m_parts[i]->records_in_range(idx, min_key, max_key);
    if (part_rows == HA_POS_ERROR) {
      return HA_POS_ERROR;
    }
    rows += part_rows;
  }
  return rows;
}

int ha_warp_part::info(uint flag) {
  /* the partitions that the statement uses (all of them before the
     partitions are pruned) */
  if (m_parts_closed || m_part_info == nullptr) {
    return 0;
  }
  ha_rows records = 0;
  ha_rows deleted = 0;
  ulonglong data_file_length = 0;
  ulong mean_rec_length = 0;
  for (uint i = m_part_info->get_first_used_partition(); i < MY_BIT_NONE;
       i = m_part_info->get_next_used_partition(i)) {
    ha_warp *part = m_parts[i].get();
    part->info(flag);
    records += part->stats.records;
    deleted += part->stats.deleted;
    data_file_length += part->stats.data_file_length;
    mean_rec_length = std::max(mean_rec_length, part->stats.mean_rec_length);
  }
  stats.records = records;
  stats.deleted = deleted;
  stats.data_file_length = data_file_length;
  stats.mean_rec_length = mean_rec_length;
  return 0;
}

int ha_warp_part::records(ha_rows *num_rows) {
  *num_rows = 0;
  for (uint i = m_part_info->get_first_used_partition(); i < MY_BIT_NONE;
       i = m_part_info->get_next_used_partition(i)) {
    ha_rows part_rows = 0;
    const int rc = m_parts[i]->records(&part_rows);
    if (rc != 0) {
      return rc;
    }
    *num_rows += part_rows;
  }
  return 0;
}

int ha_warp_part::check(THD *thd, HA_CHECK_OPT *check_opt) {
  for (auto &part : m_parts) {
    const int rc = part->check(thd, check_opt);
    if (rc != HA_ADMIN_OK) return rc;
  }
  return HA_ADMIN_OK;
}

int ha_warp_part::repair(THD *thd, HA_CHECK_OPT *check_opt) {
  for (auto &part : m_parts) {
    const int rc = part->repair(thd, check_opt);
    if (rc != HA_ADMIN_OK) return rc;
  }
  return HA_ADMIN_OK;
}

int ha_warp_part::optimize(THD *thd, HA_CHECK_OPT *check_opt) {
  for (auto &part : m_parts) {
    const int rc = part->optimize(thd, check_opt);
    if (rc != HA_ADMIN_OK) return rc;
  }
  return HA_ADMIN_OK;
}

void ha_warp_part::print_error(int error, myf errflag) {
  if (print_partition_error(error)) {
    handler::print_error(error, errflag);
  }
}

/* Changes of the partitioning.  DROP PARTITION, and ADD PARTITION of RANGE
   and LIST partitioning, are made in place: the data directories of the
   partitions are removed or created, the other partitions are not read.
   The other changes copy the table. */

uint ha_warp_part::alter_flags(uint flags) const {
  static const uint copy_changes =
      Alter_info::ALTER_COALESCE_PARTITION |
      Alter_info::ALTER_REORGANIZE_PARTITION | Alter_info::ALTER_TABLE_REORG |
      Alter_info::ALTER_REBUILD_PARTITION |
      Alter_info::ALTER_REMOVE_PARTITIONING |
      Alter_info::ALTER_EXCHANGE_PARTITION;
  uint ret = HA_PARTITION_FUNCTION_SUPPORTED;
  if ((flags & copy_changes) == 0) {
    if (flags & Alter_info::ALTER_DROP_PARTITION) {
      ret |= HA_INPLACE_CHANGE_PARTITION;
    } else if ((flags & Alter_info::ALTER_ADD_PARTITION) &&
               m_part_info != nullptr &&
               (m_part_info->part_type == partition_type::RANGE ||
                m_part_info->part_type == partition_type::LIST)) {
      ret |= HA_INPLACE_CHANGE_PARTITION;
    }
  }
  return ret;
}

static const Alter_inplace_info::HA_ALTER_FLAGS warp_partition_changes =
    Alter_inplace_info::ADD_PARTITION | Alter_inplace_info::DROP_PARTITION |
    Alter_inplace_info::ALTER_PARTITION |
    Alter_inplace_info::COALESCE_PARTITION |
    Alter_inplace_info::REORGANIZE_PARTITION |
    Alter_inplace_info::ALTER_TABLE_REORG |
    Alter_inplace_info::ALTER_REMOVE_PARTITIONING |
    Alter_inplace_info::ALTER_ALL_PARTITION |
    Alter_inplace_info::ALTER_REBUILD_PARTITION;

enum_alter_inplace_result ha_warp_part::check_if_supported_inplace_alter(
    TABLE *altered_table, Alter_inplace_info *ha_alter_info) {
  const Alter_inplace_info::HA_ALTER_FLAGS flags =
      ha_alter_info->handler_flags;
  if ((flags & warp_partition_changes) == 0) {
    /* the other changes are the ones of a table that is not partitioned
       (the indexes are declarations) */
    if (m_parts.empty() || m_parts_closed) {
      return HA_ALTER_INPLACE_NOT_SUPPORTED;
    }
    return m_parts[0]->check_if_supported_inplace_alter(altered_table,
                                                        ha_alter_info);
  }
  /* one change of the partitioning, and nothing else */
  if (flags == Alter_inplace_info::DROP_PARTITION) {
    return HA_ALTER_INPLACE_EXCLUSIVE_LOCK;
  }
  if (flags == Alter_inplace_info::ADD_PARTITION &&
      (m_part_info->part_type == partition_type::RANGE ||
       m_part_info->part_type == partition_type::LIST)) {
    return HA_ALTER_INPLACE_EXCLUSIVE_LOCK;
  }
  return HA_ALTER_INPLACE_NOT_SUPPORTED;
}

bool ha_warp_part::inplace_alter_table(TABLE *, Alter_inplace_info *,
                                       const dd::Table *, dd::Table *) {
  /* the partitions are created and removed when the change is committed */
  return false;
}

bool ha_warp_part::commit_inplace_alter_table(TABLE *altered_table,
                                              Alter_inplace_info *ha_alter_info,
                                              bool commit,
                                              const dd::Table *,
                                              dd::Table *) {
  if (!commit ||
      (ha_alter_info->handler_flags & warp_partition_changes) == 0) {
    return false;
  }
  partition_info *part_info = ha_alter_info->modified_part_info;
  if (part_info == nullptr) {
    return false;
  }
  /* The partitions in their new state: the ones to be dropped are removed
     from disk, the ones to be added are created.  The table is opened again
     after the change, the partitions of this handler are closed first: the
     files of a partition that is open must not be removed. */
  if (!m_parts_closed) {
    for (auto &part : m_parts) {
      part->ha_close();
    }
    m_parts_closed = true;
  }
  const char *table_path = table_share->normalized_path.str;
  List_iterator<partition_element> part_it(part_info->partitions);
  partition_element *part_elem;
  while ((part_elem = part_it++) != nullptr) {
    const bool drop = part_elem->part_state == PART_TO_BE_DROPPED;
    const bool add = part_elem->part_state == PART_TO_BE_ADDED;
    if (!drop && !add) {
      continue;
    }
    std::vector<std::string> paths;
    if (part_info->is_sub_partitioned()) {
      List_iterator<partition_element> sub_it(part_elem->subpartitions);
      partition_element *sub_elem;
      while ((sub_elem = sub_it++) != nullptr) {
        paths.push_back(warp_partition_path(
            table_path, part_elem->partition_name, sub_elem->partition_name));
      }
    } else {
      paths.push_back(
          warp_partition_path(table_path, part_elem->partition_name, nullptr));
    }
    for (const std::string &path : paths) {
      ha_warp part(ht, altered_table->s);
      if (drop) {
        if (part.delete_table(path.c_str(), nullptr) != 0) {
          my_error(ER_INTERNAL_ERROR, MYF(0),
                   ("WARP could not remove the partition " + path).c_str());
          return true;
        }
      } else {
        const int rc = part.create(path.c_str(), altered_table,
                                   ha_alter_info->create_info, nullptr);
        if (rc != 0) {
          my_error(ER_INTERNAL_ERROR, MYF(0),
                   ("WARP could not create the partition " + path).c_str());
          return true;
        }
      }
    }
  }
  /* For an engine without atomic DDL the server opens the table again after
     the change (mysql_inplace_alter_table), with the partitions that the
     statement names (the ones that were dropped) as the partitions to use:
     the open fails because they do not exist any more.  The names are not
     used after the change. */
  if (ha_alter_info->handler_flags & Alter_inplace_info::DROP_PARTITION) {
    const_cast<Alter_info *>(ha_alter_info->alter_info)
        ->partition_names.clear();
  }
  return false;
}
