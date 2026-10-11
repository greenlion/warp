/* Background purge of old row versions and of rolled back transactions.

   A row of a WARP table is never changed in place.  A DELETE sets the bit of
   the row in the delete bitmap when the transaction commits, an UPDATE does
   that and inserts the new version of the row, and the rows that a
   transaction inserted stay in the table when the transaction rolls back
   (its bit in the commit bitmap is never set, so nobody sees them).  The
   files of the table only grow.

   The purge removes the rows that nobody can see any more from the
   partitions that have enough of them:

   - a row that is in the delete bitmap and has no history lock.  The
     history lock keeps the row visible for the transactions that are older
     than the one that deleted it, and it is removed when none of those
     exists any more (warp_global_data::cleanup_history_locks).  This
     includes the rows of the statements that were rolled back.
   - a row of a transaction that is not committed, is not open and does not
     exist any more: it rolled back, or the server stopped before it
     committed.

   A partition is purged by writing the rows that stay to a new directory
   (next to the data directory of the table, so that FastBit does not see it
   as a partition) while the rest of the table, and the partition itself, can
   be read and (after the partition is read) written.  Then the directory of
   the partition is replaced, which is what the lock of the partition is held
   for.  If rows were written to the partition in between, the new directory
   is thrown away and the partition is purged again by a later run.  The
   files of the partitions that are not purged are not touched.  A server
   that stops while the directories are replaced is repaired when the purge
   runs the next time (warp_purge_recover):

     <table>.data/p3                the partition
     <table>.data.purge-p3          the new directory, not yet in place
     <table>.data.purge-p3.old      the partition that was replaced

   The indexes of a purged partition are made again by the queries that
   need them, the row numbers of the partition changed. */
#ifndef WARP_PURGE_H
#define WARP_PURGE_H

#include <sys/stat.h>
#include <condition_variable>
#include <fstream>
#include <set>
#include <filesystem>
#include <thread>

static std::atomic<uint64_t> warp_purged_rows_total{0};
static std::atomic<uint64_t> warp_purged_partitions_total{0};
static std::atomic<uint64_t> warp_purge_failures_total{0};

/* true while the transaction exists, a statement of it may be running */
bool warp_global_data::is_trx_active(uint64_t trx_id) {
  std::lock_guard<std::mutex> guard(active_trx_mtx);
  return active_trx.find(trx_id) != active_trx.end();
}

/* Can the row (its row id and the transaction that inserted it) be removed
   from the table: does nobody see it any more? */
bool warp_global_data::is_row_purgeable(uint64_t rowid, uint64_t trx_id) {
  if(delete_bitmap->is_set(rowid)) {
    /* deleted.  The transactions older than the deleting one see it as
       long as the history lock exists. */
    return get_history_lock(rowid) == 0;
  }
  /* the rows of the transactions that were never recovered are left alone */
  if(trx_id == 0) return false;
  /* The transaction is not finished (it may commit) if it is open or if it
     exists (a transaction is destroyed after it committed or rolled back).
     This is looked at before the commit bitmap, so a transaction that
     commits at this moment is not taken for one that rolled back. */
  if(is_transaction_open(trx_id) || is_trx_active(trx_id)) return false;
  return !is_trx_committed(trx_id);
}

/* what identifies the files of a partition that was read: if it is the same
   when the new directory is put in place nothing was written to it */
struct warp_part_signature {
  bool valid = false;
  ino_t dir_inode = 0;
  off_t meta_size = 0;
  struct timespec meta_mtime = {0, 0};
  off_t r_size = 0;
  off_t t_size = 0;
  bool operator==(const warp_part_signature &o) const {
    return valid && o.valid && dir_inode == o.dir_inode &&
           meta_size == o.meta_size &&
           meta_mtime.tv_sec == o.meta_mtime.tv_sec &&
           meta_mtime.tv_nsec == o.meta_mtime.tv_nsec &&
           r_size == o.r_size && t_size == o.t_size;
  }
};

static warp_part_signature warp_partition_signature(const std::string &part_dir) {
  warp_part_signature sig;
  struct stat st;
  if(stat(part_dir.c_str(), &st) != 0) return sig;
  sig.dir_inode = st.st_ino;
  if(stat((part_dir + "/-part.txt").c_str(), &st) != 0) return sig;
  sig.meta_size = st.st_size;
  sig.meta_mtime = st.st_mtim;
  if(stat((part_dir + "/r").c_str(), &st) != 0) return sig;
  sig.r_size = st.st_size;
  if(stat((part_dir + "/t").c_str(), &st) != 0) return sig;
  sig.t_size = st.st_size;
  sig.valid = true;
  return sig;
}

/* The share of an open table has the partitions and their sizes of the
   moment that the first row was written */
static void warp_forget_partitions_of_dir(const std::string &data_dir) {
  mysql_mutex_lock(&warp_mutex);
  for(const auto &entry : *warp_open_tables) {
    WARP_SHARE *share = entry.second;
    if(data_dir == share->data_dir_name) {
      mysql_mutex_lock(&share->mutex);
      share->partitions_loaded = false;
      mysql_mutex_unlock(&share->mutex);
    }
  }
  mysql_mutex_unlock(&warp_mutex);
}

/* the rows of the column that are in keep are written to the writer */
static bool warp_copy_column(const ibis::column *col, const ibis::bitvector &keep,
                             uint32_t offset, uint32_t kept, ibis::tablex *writer) {
  const char *name = col->name();
  switch(col->type()) {
    case ibis::BYTE: {
      std::unique_ptr<ibis::array_t<signed char>> v(col->selectBytes(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::UBYTE: {
      std::unique_ptr<ibis::array_t<unsigned char>> v(col->selectUBytes(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::SHORT: {
      std::unique_ptr<ibis::array_t<int16_t>> v(col->selectShorts(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::USHORT: {
      std::unique_ptr<ibis::array_t<uint16_t>> v(col->selectUShorts(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::INT: {
      std::unique_ptr<ibis::array_t<int32_t>> v(col->selectInts(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::UINT: {
      std::unique_ptr<ibis::array_t<uint32_t>> v(col->selectUInts(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::LONG: {
      std::unique_ptr<ibis::array_t<int64_t>> v(col->selectLongs(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::ULONG: {
      std::unique_ptr<ibis::array_t<uint64_t>> v(col->selectULongs(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::FLOAT: {
      std::unique_ptr<ibis::array_t<float>> v(col->selectFloats(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::DOUBLE: {
      std::unique_ptr<ibis::array_t<double>> v(col->selectDoubles(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v->begin()) == 0;
    }
    case ibis::TEXT:
    case ibis::CATEGORY: {
      std::unique_ptr<std::vector<std::string>> v(col->selectStrings(keep));
      return v && v->size() == kept && writer->append(name, offset, offset + kept, v.get()) == 0;
    }
    default:
      return false;
  }
}

/* removes a directory and what is in it, nothing if it does not exist */
static void warp_remove_dir(const std::string &dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  if(ec) {
    sql_print_warning("WARP purge: could not remove %s: %s", dir.c_str(), ec.message().c_str());
  }
}

/* One purge or merge of a table at a time: the directories that they make
   and replace are found by the repair of an interrupted one. */
static std::mutex warp_rewrite_mtx;

/* The file in a merged partition that names the partitions that were merged
   into it.  It is there from the moment the partition is in place until the
   partitions that were merged are removed, see warp_purge_recover. */
static const char *warp_merged_marker = "-merged.txt";

/* Repairs what a server that stopped while partitions were rewritten left:

     <table>.data.purge-p3          a new directory, not in place
     <table>.data.purge-p3.old      the partition that was replaced

   A partition that was moved away is the partition again, unless the
   partition that took its rows is in place.  That one has a file that names
   the partitions that were merged into it.  Called with warp_rewrite_mtx. */
static void warp_purge_recover(const std::string &data_dir) {
  const std::string prefix = data_dir + ".purge-";
  const size_t slash = prefix.rfind('/');
  const std::string parent = slash == std::string::npos ? "." : prefix.substr(0, slash);
  const std::string base = prefix.substr(slash == std::string::npos ? 0 : slash + 1);
  std::error_code ec;
  std::vector<std::string> names;
  for(const auto &entry : std::filesystem::directory_iterator(parent, ec)) {
    const std::string name = entry.path().filename().string();
    if(name.compare(0, base.size(), base) == 0) names.push_back(name);
  }
  if(names.empty()) return;

  /* the partitions that were merged into another one that is in place */
  std::set<uint64_t> merged;
  std::vector<std::string> markers;
  for(const auto &entry : std::filesystem::directory_iterator(data_dir, ec)) {
    uint64_t number;
    if(!warp_partition_number(entry.path().filename().c_str(), number)) continue;
    const std::string marker = entry.path().string() + "/" + warp_merged_marker;
    std::ifstream in(marker);
    if(!in) continue;
    markers.push_back(marker);
    uint64_t from;
    while(in >> from) merged.insert(from);
  }

  for(const auto &name : names) {
    const std::string dir = parent + "/" + name;
    const bool is_old = name.size() > 4 && name.compare(name.size() - 4, 4, ".old") == 0;
    const std::string part_name = is_old ? name.substr(base.size(), name.size() - base.size() - 4)
                                         : name.substr(base.size());
    const std::string part_dir = data_dir + "/" + part_name;
    uint64_t number = 0;
    warp_partition_number(part_name.c_str(), number);
    struct stat st;
    if(is_old) {
      if(stat(part_dir.c_str(), &st) == 0 || merged.count(number) != 0) {
        /* the new partition is in place */
        warp_remove_dir(dir);
      } else if(rename(dir.c_str(), part_dir.c_str()) == 0) {
        /* the partition was moved away and the new one was not put in place */
        sql_print_warning("WARP purge: restored the partition %s", part_dir.c_str());
        ibis::fileManager::instance().flushDir(part_dir.c_str());
        warp_forget_partitions_of_dir(data_dir);
      }
    } else if(stat((dir + ".old").c_str(), &st) != 0) {
      /* a new directory that was not put in place */
      warp_remove_dir(dir);
    }
  }
  /* the partitions that were merged are removed or put back now */
  for(const auto &marker : markers) {
    std::error_code ec2;
    std::filesystem::remove(marker, ec2);
  }
  warp_forget_partitions_of_dir(data_dir);
}

/* Reserves partitions of a table for a merge: none of them can be chosen by a
   writer from now on, and it fails if a writer has chosen one already.  The
   reservation is given up when the object goes away. */
class warp_reservation {
 public:
  warp_reservation(const std::string &data_dir, const std::vector<uint64_t> &numbers)
      : data_dir_(data_dir), numbers_(numbers) {
    mysql_mutex_lock(&warp_mutex);
    std::vector<WARP_SHARE *> shares;
    for(const auto &entry : *warp_open_tables) {
      if(data_dir_ == entry.second->data_dir_name) shares.push_back(entry.second);
    }
    for(auto share : shares) mysql_mutex_lock(&share->mutex);
    bool free = true;
    for(auto share : shares) {
      for(uint64_t n : numbers_) {
        if(share->partitions_writing.count(n) != 0) free = false;
      }
    }
    if(free) {
      std::lock_guard<std::mutex> guard(warp_reserved_mtx);
      for(uint64_t n : numbers_) warp_reserved_partitions.insert(std::make_pair(data_dir_, n));
      held_ = true;
    }
    for(auto it = shares.rbegin(); it != shares.rend(); ++it) mysql_mutex_unlock(&(*it)->mutex);
    mysql_mutex_unlock(&warp_mutex);
  }
  ~warp_reservation() {
    if(!held_) return;
    std::lock_guard<std::mutex> guard(warp_reserved_mtx);
    for(uint64_t n : numbers_) warp_reserved_partitions.erase(std::make_pair(data_dir_, n));
  }
  bool held() const { return held_; }
  warp_reservation(const warp_reservation &) = delete;
  warp_reservation &operator=(const warp_reservation &) = delete;

 private:
  std::string data_dir_;
  std::vector<uint64_t> numbers_;
  bool held_ = false;
};

/* The partitions of a table that are read and rewritten as one */
struct warp_rewrite_source {
  uint64_t number = 0;
  std::string dir;
  warp_part_signature signature;
  std::unique_ptr<ibis::part> part;
  ibis::bitvector keep;
  uint32_t rows = 0;
  uint32_t kept = 0;
};

/* Rewrites the partitions in numbers (ascending) into one partition, the
   first of them: the rows that nobody can see are not written, and the rows
   of the others follow those of the first one.  A single partition is
   purged: it is only rewritten if at least threshold percent of its rows can
   be removed.  Several are merged, whatever there is to remove.  Returns the
   number of rows that were not written.  Called with warp_rewrite_mtx. */
static uint64_t warp_rewrite_partitions(const std::string &data_dir,
                                        const std::vector<uint64_t> &numbers,
                                        unsigned threshold) {
  if(numbers.empty()) return 0;
  const bool merge = numbers.size() > 1;
  /* a merge replaces partitions that writers may choose */
  std::unique_ptr<warp_reservation> reservation;
  if(merge) {
    reservation.reset(new warp_reservation(data_dir, numbers));
    if(!reservation->held()) return 0;
  }
  const std::string first_name = "p" + std::to_string(numbers[0]);
  const std::string first_dir = data_dir + "/" + first_name;
  const std::string stage_dir = data_dir + ".purge-" + first_name;
  std::vector<warp_rewrite_source> sources(numbers.size());
  uint64_t removable = 0;
  uint64_t total_rows = 0;
  uint32_t kept_rows = 0;
  {
    /* the partitions are read, the writers wait for the locks, in order */
    std::vector<std::unique_ptr<warp_partition_read_lock>> read_locks;
    for(size_t n = 0; n < numbers.size(); ++n) {
      auto &src = sources[n];
      src.number = numbers[n];
      src.dir = data_dir + "/p" + std::to_string(numbers[n]);
      read_locks.emplace_back(new warp_partition_read_lock(src.dir));
      src.signature = warp_partition_signature(src.dir);
      if(!src.signature.valid) return 0;
      ibis::partList parts;
      ibis::util::gatherParts(parts, src.dir.c_str(), true);
      for(auto p : parts) {
        if(!src.part && p->currentDataDir() != nullptr && src.dir == p->currentDataDir()) {
          src.part.reset(p);
        } else {
          delete p;
        }
      }
      if(!src.part || src.part->nRows() == 0) return 0;
      src.rows = src.part->nRows();
      total_rows += src.rows;
    }
    /* the history locks of the transactions that are gone do not hold rows
       any more */
    warp_state->cleanup_history_locks();
    for(auto &src : sources) {
      const ibis::column *rcol = src.part->getColumn("r");
      const ibis::column *tcol = src.part->getColumn("t");
      if(rcol == nullptr || tcol == nullptr) return 0;
      src.keep.set(1, src.rows);
      std::unique_ptr<ibis::array_t<uint64_t>> rowids(rcol->selectULongs(src.keep));
      std::unique_ptr<ibis::array_t<uint64_t>> trxs(tcol->selectULongs(src.keep));
      if(!rowids || !trxs || rowids->size() != src.rows || trxs->size() != src.rows) return 0;
      uint32_t gone = 0;
      for(uint32_t i = 0; i < src.rows; ++i) {
        if(warp_state->is_row_purgeable((*rowids)[i], (*trxs)[i])) {
          src.keep.setBit(i, 0);
          ++gone;
        }
      }
      src.keep.adjustSize(0, src.rows);
      src.kept = src.rows - gone;
      kept_rows += src.kept;
      removable += gone;
    }
    if(!merge && (removable == 0 || removable * 100 < total_rows * threshold)) {
      ibis::fileManager::instance().flushDir(first_dir.c_str());
      return 0;
    }

    warp_remove_dir(stage_dir);
    if(kept_rows > 0) {
      std::unique_ptr<ibis::tablex> writer(ibis::tablex::create());
      const ibis::part &model = *sources[0].part;
      for(uint32_t c = 0; c < model.nColumns(); ++c) {
        const ibis::column *col = model.getColumn(c);
        if(writer->addColumn(col->name(), col->type(), col->description(),
                             col->indexSpec()) != 0) {
          return 0;
        }
      }
      uint32_t offset = 0;
      for(auto &src : sources) {
        if(src.kept > 0) {
          for(uint32_t c = 0; c < src.part->nColumns(); ++c) {
            const ibis::column *col = src.part->getColumn(c);
            if(!warp_copy_column(col, src.keep, offset, src.kept, writer.get())) {
              sql_print_warning("WARP purge: could not copy the column %s of %s",
                                col->name(), src.dir.c_str());
              warp_purge_failures_total++;
              return 0;
            }
          }
        }
        offset += src.kept;
      }
      if(writer->write(stage_dir.c_str(), first_name.c_str()) != (int)kept_rows) {
        sql_print_warning("WARP purge: could not write %s", stage_dir.c_str());
        warp_remove_dir(stage_dir);
        warp_purge_failures_total++;
        return 0;
      }
      if(merge) {
        std::ofstream marker(stage_dir + "/" + warp_merged_marker);
        for(size_t n = 1; n < numbers.size(); ++n) marker << numbers[n] << "\n";
      }
    }
    for(auto &src : sources) ibis::fileManager::instance().flushDir(src.dir.c_str());
  }

  /* Put the new directory in place, with the partitions locked in order.
     A merge waits for the scans that listed the partitions to end. */
  bool swapped = false;
  std::vector<std::string> old_dirs(sources.size());
  auto table_locks = warp_table_locks(data_dir);
  const bool gate = merge && table_locks->scans.enter_exclusive(std::chrono::seconds(5));
  if(merge && !gate) {
    warp_remove_dir(stage_dir);
    return 0;
  }
  {
    std::vector<std::unique_ptr<warp_partition_write_lock>> write_locks;
    for(auto &src : sources) write_locks.emplace_back(new warp_partition_write_lock(src.dir));
    bool unchanged = true;
    for(auto &src : sources) {
      if(!(warp_partition_signature(src.dir) == src.signature)) unchanged = false;
    }
    if(unchanged) {
      size_t moved = 0;
      bool ok = true;
      /* the others first, the first one last: if the server stops before
         the new partition is in place all of them are put back */
      for(size_t n = sources.size(); n-- > 0 && ok;) {
        old_dirs[n] = data_dir + ".purge-p" + std::to_string(sources[n].number) + ".old";
        if(rename(sources[n].dir.c_str(), old_dirs[n].c_str()) == 0) {
          ++moved;
        } else {
          ok = false;
        }
      }
      if(ok && kept_rows > 0 && rename(stage_dir.c_str(), first_dir.c_str()) != 0) {
        ok = false;
      }
      if(ok) {
        swapped = true;
      } else {
        for(size_t n = sources.size(); n-- > sources.size() - moved;) {
          rename(old_dirs[n].c_str(), sources[n].dir.c_str());
        }
      }
    }
    for(auto &src : sources) ibis::fileManager::instance().flushDir(src.dir.c_str());
  }
  if(gate) table_locks->scans.leave_exclusive();
  if(!swapped) {
    /* rows were written to a partition, or it is gone */
    warp_remove_dir(stage_dir);
    return 0;
  }
  for(size_t n = 0; n < sources.size(); ++n) warp_remove_dir(old_dirs[n]);
  if(merge) {
    std::error_code ec;
    std::filesystem::remove(first_dir + "/" + warp_merged_marker, ec);
  }
  warp_forget_partitions_of_dir(data_dir);
  warp_purged_rows_total += removable;
  warp_purged_partitions_total += sources.size();
  return removable;
}

/* the numbers of the partitions of a table with their rows */
static std::map<uint64_t, uint64_t> warp_partition_row_counts(const std::string &data_dir) {
  std::map<uint64_t, uint64_t> rows;
  std::error_code ec;
  for(const auto &entry : std::filesystem::directory_iterator(data_dir, ec)) {
    uint64_t number;
    if(!warp_partition_number(entry.path().filename().c_str(), number)) continue;
    ibis::partList parts;
    ibis::util::gatherParts(parts, entry.path().c_str(), true);
    for(auto p : parts) {
      if(p->currentDataDir() != nullptr && entry.path() == p->currentDataDir()) {
        rows[number] = p->nRows();
      }
      delete p;
    }
  }
  return rows;
}

/* Purges the partitions of a table.  Returns the number of rows removed. */
static uint64_t warp_purge_table(const std::string &data_dir, unsigned threshold) {
  if(warp_state == NULL) return 0;
  std::lock_guard<std::mutex> guard(warp_rewrite_mtx);
  uint64_t removed = 0;
  try {
    warp_purge_recover(data_dir);
    std::vector<uint64_t> numbers;
    std::error_code ec;
    for(const auto &entry : std::filesystem::directory_iterator(data_dir, ec)) {
      uint64_t number;
      if(warp_partition_number(entry.path().filename().c_str(), number)) {
        numbers.push_back(number);
      }
    }
    std::sort(numbers.begin(), numbers.end());
    for(uint64_t number : numbers) {
      removed += warp_rewrite_partitions(data_dir, {number}, threshold);
    }
  } catch(const std::exception &e) {
    sql_print_warning("WARP purge of %s failed: %s", data_dir.c_str(), e.what());
    warp_purge_failures_total++;
  } catch(...) {
    sql_print_warning("WARP purge of %s failed", data_dir.c_str());
    warp_purge_failures_total++;
  }
  return removed;
}

/* The partitions that rows were written to in small batches (see
   ha_warp::claim_writer_partition) are merged: runs of the partitions that
   have the rows of a tail partition or less are written as one partition, as
   long as the rows fit in a partition.  The tail partition that rows are
   written to now, the last one, is left alone.  Returns the number of
   partitions that were merged away. */
static uint64_t warp_merge_tail_partitions(const std::string &data_dir) {
  if(warp_state == NULL || my_tail_partition_rows == 0) return 0;
  std::lock_guard<std::mutex> guard(warp_rewrite_mtx);
  const uint64_t tail_rows = std::min<uint64_t>(my_tail_partition_rows, my_partition_max_rows);
  uint64_t merged_away = 0;
  try {
    warp_purge_recover(data_dir);
    const auto rows = warp_partition_row_counts(data_dir);
    /* the partitions with at most the rows of a tail partition, without the
       last partition of the table */
    std::vector<uint64_t> small;
    for(const auto &partition : rows) {
      if(partition.second > 0 && partition.second <= tail_rows &&
         partition.first != rows.rbegin()->first) {
        small.push_back(partition.first);
      }
    }
    /* runs of the partitions that fit in one */
    size_t i = 0;
    while(i < small.size()) {
      std::vector<uint64_t> group;
      uint64_t total = 0;
      while(i < small.size() && total + rows.at(small[i]) <= my_partition_max_rows) {
        total += rows.at(small[i]);
        group.push_back(small[i]);
        ++i;
      }
      if(group.empty()) {
        ++i;
        continue;
      }
      /* a few partitions are not worth the writing */
      if(group.size() >= 4) {
        const uint64_t before = warp_purged_partitions_total.load();
        warp_rewrite_partitions(data_dir, group, 0);
        if(warp_purged_partitions_total.load() > before) merged_away += group.size() - 1;
      }
    }
  } catch(const std::exception &e) {
    sql_print_warning("WARP merge of %s failed: %s", data_dir.c_str(), e.what());
    warp_purge_failures_total++;
  } catch(...) {
    sql_print_warning("WARP merge of %s failed", data_dir.c_str());
    warp_purge_failures_total++;
  }
  return merged_away;
}

/* the data directories of all the WARP tables, like ./test/t1.data */
static std::vector<std::string> warp_list_tables() {
  std::vector<std::string> tables;
  std::error_code ec;
  for(const auto &db : std::filesystem::directory_iterator(".", ec)) {
    std::error_code ec2;
    if(!db.is_directory(ec2)) continue;
    for(const auto &entry : std::filesystem::directory_iterator(db.path(), ec2)) {
      const std::string name = entry.path().filename().string();
      if(name.size() > 5 && name.compare(name.size() - 5, 5, ".data") == 0 &&
         entry.is_directory(ec2)) {
        tables.push_back(entry.path().string());
      }
    }
  }
  return tables;
}

static std::mutex warp_purge_mtx;
static std::condition_variable warp_purge_cv;
static std::atomic<bool> warp_purge_stopping{false};
static std::thread warp_purge_thread;

static void warp_purge_wake() {
  /* the lock makes sure that the thread is waiting, not between looking at
     the setting and waiting */
  std::lock_guard<std::mutex> guard(warp_purge_mtx);
  warp_purge_cv.notify_all();
}

static void warp_purge_thread_main() {
  my_thread_init();
  std::unique_lock<std::mutex> lock(warp_purge_mtx);
  auto last_run = std::chrono::steady_clock::now();
  while(!warp_purge_stopping) {
    const unsigned long interval = my_purge_interval;
    warp_purge_cv.wait_for(lock, std::chrono::seconds(interval > 0 ? interval : 5));
    if(warp_purge_stopping) break;
    const unsigned long now_interval = my_purge_interval;
    /* off, or woken up because a setting changed before the time is up */
    if(now_interval == 0 ||
       std::chrono::steady_clock::now() - last_run < std::chrono::seconds(now_interval)) {
      continue;
    }
    lock.unlock();
    for(const auto &table : warp_list_tables()) {
      if(warp_purge_stopping) break;
      warp_purge_table(table, my_purge_threshold);
      warp_merge_tail_partitions(table);
    }
    lock.lock();
    last_run = std::chrono::steady_clock::now();
  }
  lock.unlock();
  my_thread_end();
}

static void warp_purge_start() {
  warp_purge_stopping = false;
  warp_purge_thread = std::thread(warp_purge_thread_main);
}

static void warp_purge_stop() {
  {
    std::lock_guard<std::mutex> guard(warp_purge_mtx);
    warp_purge_stopping = true;
  }
  warp_purge_cv.notify_all();
  if(warp_purge_thread.joinable()) warp_purge_thread.join();
}

#endif /* WARP_PURGE_H */
