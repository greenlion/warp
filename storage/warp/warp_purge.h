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
                             uint32_t kept, ibis::tablex *writer) {
  const char *name = col->name();
  switch(col->type()) {
    case ibis::BYTE: {
      std::unique_ptr<ibis::array_t<signed char>> v(col->selectBytes(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::UBYTE: {
      std::unique_ptr<ibis::array_t<unsigned char>> v(col->selectUBytes(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::SHORT: {
      std::unique_ptr<ibis::array_t<int16_t>> v(col->selectShorts(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::USHORT: {
      std::unique_ptr<ibis::array_t<uint16_t>> v(col->selectUShorts(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::INT: {
      std::unique_ptr<ibis::array_t<int32_t>> v(col->selectInts(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::UINT: {
      std::unique_ptr<ibis::array_t<uint32_t>> v(col->selectUInts(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::LONG: {
      std::unique_ptr<ibis::array_t<int64_t>> v(col->selectLongs(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::ULONG: {
      std::unique_ptr<ibis::array_t<uint64_t>> v(col->selectULongs(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::FLOAT: {
      std::unique_ptr<ibis::array_t<float>> v(col->selectFloats(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::DOUBLE: {
      std::unique_ptr<ibis::array_t<double>> v(col->selectDoubles(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v->begin()) == 0;
    }
    case ibis::TEXT:
    case ibis::CATEGORY: {
      std::unique_ptr<std::vector<std::string>> v(col->selectStrings(keep));
      return v && v->size() == kept && writer->append(name, 0, kept, v.get()) == 0;
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

/* Repairs what a server that stopped while a partition was purged left. */
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
  for(const auto &name : names) {
    const std::string dir = parent + "/" + name;
    const bool is_old = name.size() > 4 && name.compare(name.size() - 4, 4, ".old") == 0;
    const std::string part_name = is_old ? name.substr(base.size(), name.size() - base.size() - 4)
                                         : name.substr(base.size());
    const std::string part_dir = data_dir + "/" + part_name;
    if(is_old) {
      struct stat st;
      if(stat(part_dir.c_str(), &st) != 0) {
        /* the partition was moved away and the new one was not put in place:
           the old one is the partition */
        if(rename(dir.c_str(), part_dir.c_str()) == 0) {
          sql_print_warning("WARP purge: restored the partition %s", part_dir.c_str());
          ibis::fileManager::instance().flushDir(part_dir.c_str());
          warp_forget_partitions_of_dir(data_dir);
        }
      } else {
        warp_remove_dir(dir);
      }
    } else {
      /* a new directory that was not put in place, or the one of a purge that
         is running now */
      struct stat st;
      if(stat((dir + ".old").c_str(), &st) != 0) warp_remove_dir(dir);
    }
  }
}

/* Purges one partition.  threshold is the percent of the rows of the
   partition that must be removable.  Returns the number of rows removed. */
static uint64_t warp_purge_partition(const std::string &data_dir, uint64_t number,
                                     unsigned threshold) {
  const std::string name = "p" + std::to_string(number);
  const std::string part_dir = data_dir + "/" + name;
  const std::string stage_dir = data_dir + ".purge-" + name;
  const std::string old_dir = stage_dir + ".old";
  struct stat st;
  if(stat((part_dir + "/-part.txt").c_str(), &st) != 0) return 0;

  warp_part_signature signature;
  uint32_t kept = 0;
  uint32_t rows = 0;
  {
    /* the partition is read, the writers wait for the lock */
    warp_partition_read_lock read_lock(part_dir);
    signature = warp_partition_signature(part_dir);
    if(!signature.valid) return 0;

    ibis::partList parts;
    ibis::util::gatherParts(parts, part_dir.c_str(), true);
    std::unique_ptr<ibis::part> part;
    for(auto p : parts) {
      if(!part && p->currentDataDir() != nullptr && part_dir == p->currentDataDir()) {
        part.reset(p);
      } else {
        delete p;
      }
    }
    if(!part || part->nRows() == 0) return 0;
    rows = part->nRows();
    const ibis::column *rcol = part->getColumn("r");
    const ibis::column *tcol = part->getColumn("t");
    if(rcol == nullptr || tcol == nullptr) return 0;

    ibis::bitvector keep;
    keep.set(1, rows);
    uint64_t removable = 0;
    {
      std::unique_ptr<ibis::array_t<uint64_t>> rowids(rcol->selectULongs(keep));
      std::unique_ptr<ibis::array_t<uint64_t>> trxs(tcol->selectULongs(keep));
      if(!rowids || !trxs || rowids->size() != rows || trxs->size() != rows) return 0;
      /* the history locks of the transactions that are gone do not hold
         rows any more */
      warp_state->cleanup_history_locks();
      for(uint32_t i = 0; i < rows; ++i) {
        if(warp_state->is_row_purgeable((*rowids)[i], (*trxs)[i])) {
          keep.setBit(i, 0);
          ++removable;
        }
      }
    }
    keep.adjustSize(0, rows);
    if(removable == 0 || removable * 100 < (uint64_t)rows * threshold) {
      ibis::fileManager::instance().flushDir(part_dir.c_str());
      return 0;
    }
    kept = rows - (uint32_t)removable;

    warp_remove_dir(stage_dir);
    if(kept > 0) {
      std::unique_ptr<ibis::tablex> writer(ibis::tablex::create());
      for(uint32_t c = 0; c < part->nColumns(); ++c) {
        const ibis::column *col = part->getColumn(c);
        if(writer->addColumn(col->name(), col->type(), col->description(),
                             col->indexSpec()) != 0) {
          return 0;
        }
      }
      for(uint32_t c = 0; c < part->nColumns(); ++c) {
        if(!warp_copy_column(part->getColumn(c), keep, kept, writer.get())) {
          sql_print_warning("WARP purge: could not copy the column %s of %s",
                            part->getColumn(c)->name(), part_dir.c_str());
          warp_purge_failures_total++;
          return 0;
        }
      }
      if(writer->write(stage_dir.c_str(), name.c_str()) != (int)kept) {
        sql_print_warning("WARP purge: could not write %s", stage_dir.c_str());
        warp_remove_dir(stage_dir);
        warp_purge_failures_total++;
        return 0;
      }
    }
    ibis::fileManager::instance().flushDir(part_dir.c_str());
  }

  /* put the new directory in place */
  bool swapped = false;
  {
    warp_partition_write_lock write_lock(part_dir);
    if(warp_partition_signature(part_dir) == signature) {
      if(rename(part_dir.c_str(), old_dir.c_str()) == 0) {
        if(kept == 0 || rename(stage_dir.c_str(), part_dir.c_str()) == 0) {
          swapped = true;
        } else {
          rename(old_dir.c_str(), part_dir.c_str());
        }
      }
    }
    ibis::fileManager::instance().flushDir(part_dir.c_str());
  }
  if(!swapped) {
    /* rows were written to the partition, or it is gone */
    warp_remove_dir(stage_dir);
    return 0;
  }
  warp_remove_dir(old_dir);
  warp_forget_partitions_of_dir(data_dir);
  warp_purged_rows_total += rows - kept;
  warp_purged_partitions_total++;
  return rows - kept;
}

/* Purges the partitions of a table.  Returns the number of rows removed. */
static uint64_t warp_purge_table(const std::string &data_dir, unsigned threshold) {
  if(warp_state == NULL) return 0;
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
      removed += warp_purge_partition(data_dir, number, threshold);
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
