/* Locks of the data files of WARP tables.

   A writer appends rows to the column files of a partition one column after
   the other and updates the row count of the partition.  A scan that opens
   the partition at that moment sees a row count that the column files do not
   have yet (bord::append could only add 8005 of 8006 values, error -19) and
   fails.  The purge of a partition replaces its directory.  So the code that
   reads the data files of a partition holds the lock of the partition shared,
   and the code that changes the files holds it exclusive.  The rows that a
   scan returns are in memory (a table that select made), so the lock is not
   held while they are returned.

   A partition lock is kept by the name of the directory of the partition (the
   data directory of the table and "p" and the number of the partition), so
   code that only has the name (the dimension table of a join) finds the same
   lock.  A thread that holds a lock for reading does not take it again.

   The code that reads a whole table takes the locks of all its partitions,
   in the order of their numbers (warp_table_read_lock), the others take the
   lock of the partition that they use.  A lock is only held for a partition
   at a time by the code that changes files (except for what is taken in
   order by warp_table_write_lock), so the order is enough to prevent
   deadlocks.

   Every table also has a lock of its structure.  It is held shared while a
   reader takes the locks of the partitions and exclusive while a partition
   is created and while a table is dropped or renamed.  A scan that takes the
   locks of the partitions that exist can therefore not miss a partition that
   is being created: it finds the lock of the new partition, or the creation
   waits for the scan to have its locks. */
#ifndef WARP_LOCKS_H
#define WARP_LOCKS_H

#include <dirent.h>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

/* The scans of a table that has partitions merged (see warp_purge.h) hold the
   gate shared from their start to their end: a scan lists the partitions
   once, and a merge that removes a partition under it would make it fail
   or miss rows.  The merge takes the gate exclusive, waiting for the scans
   that run to end for a while, and gives up if they do not.  While it
   waits the scans that start wait for it, so it does not starve. */
class warp_scan_gate {
 public:
  void enter() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return !writer_waiting_ && !writer_active_; });
    ++readers_;
  }
  void leave() {
    std::lock_guard<std::mutex> lock(mtx_);
    if(--readers_ == 0) cv_.notify_all();
  }
  bool enter_exclusive(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mtx_);
    /* one merge at a time waits */
    if(writer_waiting_ || writer_active_) return false;
    writer_waiting_ = true;
    const bool drained = cv_.wait_for(lock, timeout, [this] { return readers_ == 0; });
    writer_waiting_ = false;
    if(!drained) {
      cv_.notify_all();
      return false;
    }
    writer_active_ = true;
    return true;
  }
  void leave_exclusive() {
    std::lock_guard<std::mutex> lock(mtx_);
    writer_active_ = false;
    cv_.notify_all();
  }

 private:
  std::mutex mtx_;
  std::condition_variable cv_;
  unsigned readers_ = 0;
  bool writer_waiting_ = false;
  bool writer_active_ = false;
};

struct warp_table_lock_set {
  warp_scan_gate scans;
  std::shared_mutex structure;
  /* the locks of the partitions by partition number, protected by
     warp_lock_registry_mtx */
  std::map<uint64_t, std::shared_ptr<std::shared_mutex>> partitions;
  bool scanned = false;
};

static std::mutex warp_lock_registry_mtx;
static std::unordered_map<std::string, std::shared_ptr<warp_table_lock_set>>
    warp_lock_registry;

/* the number of a directory name like p17, false for other names */
static bool warp_partition_number(const char *name, uint64_t &number) {
  if(name == nullptr || name[0] != 'p' || name[1] < '0' || name[1] > '9') {
    return false;
  }
  char *end = nullptr;
  number = strtoull(name + 1, &end, 10);
  return *end == 0;
}

/* the lock of a partition (a number in the table), made if there is none */
static std::shared_ptr<std::shared_mutex> warp_partition_mutex(
    const std::shared_ptr<warp_table_lock_set> &set, uint64_t number) {
  std::lock_guard<std::mutex> guard(warp_lock_registry_mtx);
  auto it = set->partitions.find(number);
  if(it == set->partitions.end()) {
    it = set->partitions.emplace(number, std::make_shared<std::shared_mutex>()).first;
  }
  return it->second;
}

/* the locks of the table, with those of the partitions that are on disk the
   first time */
static std::shared_ptr<warp_table_lock_set> warp_table_locks(const std::string &data_dir) {
  std::lock_guard<std::mutex> guard(warp_lock_registry_mtx);
  auto it = warp_lock_registry.find(data_dir);
  if(it == warp_lock_registry.end()) {
    it = warp_lock_registry.emplace(data_dir, std::make_shared<warp_table_lock_set>()).first;
  }
  auto &set = it->second;
  if(!set->scanned) {
    DIR *dir = opendir(data_dir.c_str());
    if(dir != nullptr) {
      while(struct dirent *entry = readdir(dir)) {
        uint64_t number;
        if(warp_partition_number(entry->d_name, number) &&
           set->partitions.find(number) == set->partitions.end()) {
          set->partitions.emplace(number, std::make_shared<std::shared_mutex>());
        }
      }
      closedir(dir);
    }
    set->scanned = true;
  }
  return set;
}

/* The partitions that a merge is going to replace (see warp_purge.h).  The
   writers do not choose them, a writer that has chosen a partition and waits
   for its lock would write to the partition that took its place.  Protected
   by its own mutex, taken while the mutex of a share is held. */
static std::mutex warp_reserved_mtx;
static std::set<std::pair<std::string, uint64_t>> warp_reserved_partitions;

static bool warp_partition_reserved(const std::string &data_dir, uint64_t number) {
  std::lock_guard<std::mutex> guard(warp_reserved_mtx);
  return warp_reserved_partitions.count(std::make_pair(data_dir, number)) != 0;
}

/* does the partition have the index of a column? (the queries make them) */
static bool warp_partition_has_indexes(const std::string &part_dir) {
  DIR *dir = opendir(part_dir.c_str());
  if(dir == nullptr) return false;
  bool found = false;
  while(struct dirent *entry = readdir(dir)) {
    const size_t len = strlen(entry->d_name);
    if(len > 4 && strcmp(entry->d_name + len - 4, ".idx") == 0) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

/* The locks of a table that is dropped or renamed (by another name) are not
   used again.  Code that still holds them keeps them. */
static void warp_forget_table_locks(const std::string &data_dir) {
  std::lock_guard<std::mutex> guard(warp_lock_registry_mtx);
  warp_lock_registry.erase(data_dir);
}

/* split the directory of a partition into the data directory of the table and
   the partition number */
static bool warp_split_partition_dir(const std::string &part_dir,
                                     std::string &data_dir, uint64_t &number) {
  const size_t slash = part_dir.rfind('/');
  if(slash == std::string::npos) return false;
  data_dir = part_dir.substr(0, slash);
  return warp_partition_number(part_dir.c_str() + slash + 1, number);
}

/* the locks that the current thread holds for reading, and how often */
static thread_local std::unordered_map<const std::shared_mutex *, int> warp_read_locks_held;
/* the tables that the current thread holds the read lock of */
static thread_local std::unordered_map<const warp_table_lock_set *, int> warp_table_reads_held;

static void warp_acquire_read(const std::shared_ptr<std::shared_mutex> &m) {
  if(warp_read_locks_held[m.get()]++ == 0) {
    m->lock_shared();
  }
}

static void warp_release_read(const std::shared_ptr<std::shared_mutex> &m) {
  auto it = warp_read_locks_held.find(m.get());
  if(--(it->second) == 0) {
    warp_read_locks_held.erase(it);
    m->unlock_shared();
  }
}

/* the scans of the tables that the current thread runs, to enter the gate of
   a table once per thread */
static thread_local std::unordered_map<const warp_table_lock_set *, int> warp_scans_held;

static void warp_scan_enter(const std::shared_ptr<warp_table_lock_set> &set) {
  if(warp_scans_held[set.get()]++ == 0) set->scans.enter();
}

static void warp_scan_leave(const std::shared_ptr<warp_table_lock_set> &set) {
  auto it = warp_scans_held.find(set.get());
  if(it != warp_scans_held.end() && --(it->second) == 0) {
    warp_scans_held.erase(it);
    set->scans.leave();
  }
}

/* shared lock of the partitions of a table that exist */
class warp_table_read_lock {
 public:
  explicit warp_table_read_lock(const char *data_dir)
      : set_(warp_table_locks(data_dir)) {
    /* the thread has the table locked already */
    if(warp_table_reads_held[set_.get()]++ > 0) {
      nested_ = true;
      return;
    }
    /* No partition is created while the table is read: the metadata of a
       partition that is written is not read, and the partitions that are
       found are the ones that are locked */
    set_->structure.lock_shared();
    structure_locked_ = true;
    {
      std::lock_guard<std::mutex> guard(warp_lock_registry_mtx);
      for(const auto &partition : set_->partitions) {
        locks_.push_back(partition.second);
      }
    }
    for(const auto &m : locks_) warp_acquire_read(m);
  }
  ~warp_table_read_lock() {
    for(auto it = locks_.rbegin(); it != locks_.rend(); ++it) warp_release_read(*it);
    if(structure_locked_) set_->structure.unlock_shared();
    auto held = warp_table_reads_held.find(set_.get());
    if(--(held->second) == 0) warp_table_reads_held.erase(held);
  }
  warp_table_read_lock(const warp_table_read_lock &) = delete;
  warp_table_read_lock &operator=(const warp_table_read_lock &) = delete;

 private:
  std::shared_ptr<warp_table_lock_set> set_;
  std::vector<std::shared_ptr<std::shared_mutex>> locks_;
  bool nested_ = false;
  bool structure_locked_ = false;
};

/* shared lock of one partition, for the code that reads one partition */
class warp_partition_read_lock {
 public:
  explicit warp_partition_read_lock(const std::string &part_dir) {
    std::string data_dir;
    uint64_t number;
    if(!warp_split_partition_dir(part_dir, data_dir, number)) return;
    lock_ = warp_partition_mutex(warp_table_locks(data_dir), number);
    warp_acquire_read(lock_);
  }
  ~warp_partition_read_lock() {
    if(lock_) warp_release_read(lock_);
  }
  warp_partition_read_lock(const warp_partition_read_lock &) = delete;
  warp_partition_read_lock &operator=(const warp_partition_read_lock &) = delete;

 private:
  std::shared_ptr<std::shared_mutex> lock_;
};

/* exclusive lock of one partition.  creating is true if the partition is
   made by the code that takes the lock: its structure is then locked while
   the lock of the partition is made. */
class warp_partition_write_lock {
 public:
  explicit warp_partition_write_lock(const std::string &part_dir, bool creating = false) {
    std::string data_dir;
    uint64_t number;
    if(!warp_split_partition_dir(part_dir, data_dir, number)) return;
    set_ = warp_table_locks(data_dir);
    /* A partition that is made is not found by a scan before it is written,
       the table is not read until it is complete */
    if(creating && warp_table_reads_held.find(set_.get()) == warp_table_reads_held.end()) {
      set_->structure.lock();
      structure_locked_ = true;
    }
    lock_ = warp_partition_mutex(set_, number);
    /* a thread that reads can not wait for the writers, it would wait
       for itself */
    if(warp_read_locks_held.find(lock_.get()) == warp_read_locks_held.end()) {
      lock_->lock();
      locked_ = true;
    } else {
      sql_print_warning("WARP: rows are written by a thread that is reading %s",
                        part_dir.c_str());
    }
  }
  ~warp_partition_write_lock() {
    if(locked_) lock_->unlock();
    if(structure_locked_) set_->structure.unlock();
  }
  warp_partition_write_lock(const warp_partition_write_lock &) = delete;
  warp_partition_write_lock &operator=(const warp_partition_write_lock &) = delete;

 private:
  std::shared_ptr<warp_table_lock_set> set_;
  std::shared_ptr<std::shared_mutex> lock_;
  bool locked_ = false;
  bool structure_locked_ = false;
};

/* exclusive lock of all the partitions of a table and of its structure: the
   table is dropped or renamed */
class warp_table_write_lock {
 public:
  explicit warp_table_write_lock(const char *data_dir)
      : set_(warp_table_locks(data_dir)) {
    /* a thread that reads can not wait for the writers */
    if(warp_table_reads_held.find(set_.get()) != warp_table_reads_held.end()) {
      sql_print_warning("WARP: the table %s is changed by a thread that is reading it", data_dir);
      return;
    }
    set_->structure.lock();
    structure_locked_ = true;
    {
      std::lock_guard<std::mutex> guard(warp_lock_registry_mtx);
      for(const auto &partition : set_->partitions) {
        locks_.push_back(partition.second);
      }
    }
    for(const auto &m : locks_) {
      if(warp_read_locks_held.find(m.get()) == warp_read_locks_held.end()) {
        m->lock();
        locked_.push_back(m);
      }
    }
  }
  ~warp_table_write_lock() {
    for(auto it = locked_.rbegin(); it != locked_.rend(); ++it) (*it)->unlock();
    if(structure_locked_) set_->structure.unlock();
  }
  warp_table_write_lock(const warp_table_write_lock &) = delete;
  warp_table_write_lock &operator=(const warp_table_write_lock &) = delete;

 private:
  std::shared_ptr<warp_table_lock_set> set_;
  std::vector<std::shared_ptr<std::shared_mutex>> locks_;
  std::vector<std::shared_ptr<std::shared_mutex>> locked_;
  bool structure_locked_ = false;
};

#endif /* WARP_LOCKS_H */
