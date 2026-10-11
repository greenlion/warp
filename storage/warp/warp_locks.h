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
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct warp_table_lock_set {
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
    set_->structure.lock_shared();
    {
      std::lock_guard<std::mutex> guard(warp_lock_registry_mtx);
      for(const auto &partition : set_->partitions) {
        locks_.push_back(partition.second);
      }
    }
    for(const auto &m : locks_) warp_acquire_read(m);
    set_->structure.unlock_shared();
  }
  ~warp_table_read_lock() {
    for(auto it = locks_.rbegin(); it != locks_.rend(); ++it) warp_release_read(*it);
    auto held = warp_table_reads_held.find(set_.get());
    if(--(held->second) == 0) warp_table_reads_held.erase(held);
  }
  warp_table_read_lock(const warp_table_read_lock &) = delete;
  warp_table_read_lock &operator=(const warp_table_read_lock &) = delete;

 private:
  std::shared_ptr<warp_table_lock_set> set_;
  std::vector<std::shared_ptr<std::shared_mutex>> locks_;
  bool nested_ = false;
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
    auto set = warp_table_locks(data_dir);
    if(creating) set->structure.lock();
    lock_ = warp_partition_mutex(set, number);
    /* a thread that reads can not wait for the writers, it would wait
       for itself */
    if(warp_read_locks_held.find(lock_.get()) == warp_read_locks_held.end()) {
      lock_->lock();
      locked_ = true;
    } else {
      sql_print_warning("WARP: rows are written by a thread that is reading %s",
                        part_dir.c_str());
    }
    if(creating) set->structure.unlock();
  }
  ~warp_partition_write_lock() {
    if(locked_) lock_->unlock();
  }
  warp_partition_write_lock(const warp_partition_write_lock &) = delete;
  warp_partition_write_lock &operator=(const warp_partition_write_lock &) = delete;

 private:
  std::shared_ptr<std::shared_mutex> lock_;
  bool locked_ = false;
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
