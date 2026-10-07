/* Copyright (C)  2020 Justin Swanhart

	 This program is free software; you can redistribute it and/or modify
	 it under the terms of the GNU General Public License version 2.0 as
	 published by the Free  Software Foundation.

	 This program is distributed in the hope that  it will be useful, but
	 WITHOUT ANY WARRANTY; without even  the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	 General Public License version 2.0 for more details.

	 You should have received a  copy of the GNU General Public License
	 version 2.0  along with this  program; if not, write to the Free
	 Software Foundation,  Inc., 59 Temple Place, Suite 330, Boston, MA
	 02111-1307 USA  */

#ifndef WARP_SPARSE_HEADER
#define WARP_SPARSE_HEADER
#include <stdio.h>
#include <string>
#include <iostream>
#include <unistd.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <stdint.h>
#include <assert.h>

#define MODE_SET 1
#define MODE_UNSET 0
// 64 bits
#define BLOCK_SIZE 8
#define MAX_BITS 64

//#ifdef WARP_BITMAP_DEBUG
#define bitmap_dbug(x) std::cerr << __LINE__ << ": " << x << "\n"; 
//#else
//#define //bitmap_dbug(x) /* would write (x) to debug log*/
//#endif

class sparsebitmap
{
private:
  int line;
  int dirty=0;
  int have_lock=LOCK_UN;
  unsigned long long fpos = 0;

  /* The read path (is_set) does not use the FILE of the writer.  It reads
     through a read only memory map of the file, so that a lookup is a load
     from memory, not a seek and a read.  A map covers the whole blocks of the
     file at the time it was made.  Writers (set_bit, commit, close, open)
     change the file and then bump read_generation, which makes the readers
     map the file again the next time they look a bit up.  The old maps stay
     around until the object is destroyed because another thread may still
     read from one, so a lookup needs no lock.  A bit past the end of the
     map is not set. */
  struct read_map {
    const uint64_t *base;
    size_t len;          /* bytes, a multiple of BLOCK_SIZE */
    uint64_t generation; /* read_generation when the map was made */
  };
  std::atomic<read_map*> rmap{NULL};
  std::atomic<uint64_t> read_generation{1};
  std::mutex rmap_mtx;
  std::vector<read_map*> rmap_all;      /* every map ever made, freed at the end */
  std::vector<std::pair<void*, size_t>> rmap_mappings;
  std::atomic<bool> rmap_failed{false}; /* mmap did not work, use the FILE */
  std::mutex slow_mtx;                  /* the FILE path is shared by all threads */

  /* Make a new map of the file if the file changed since the last one. */
  const read_map *refresh_read_map() {
    std::lock_guard<std::mutex> guard(rmap_mtx);
    const uint64_t gen = read_generation.load(std::memory_order_acquire);
    read_map *cur = rmap.load(std::memory_order_acquire);
    if(cur != NULL && cur->generation == gen) {
      return cur;
    }
    const uint64_t *base = NULL;
    size_t len = 0;
    int fd = ::open(fname.c_str(), O_RDONLY);
    if(fd >= 0) {
      struct stat st;
      if(fstat(fd, &st) == 0 && st.st_size >= (off_t)BLOCK_SIZE) {
        len = ((size_t)st.st_size / BLOCK_SIZE) * BLOCK_SIZE;
        void *addr = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
        if(addr == MAP_FAILED) {
          len = 0;
          rmap_failed = true;
        } else {
          base = static_cast<const uint64_t *>(addr);
          rmap_mappings.emplace_back(addr, len);
        }
      }
      ::close(fd);
    } else {
      rmap_failed = true;
    }
    read_map *fresh = new read_map{base, len, gen};
    rmap_all.push_back(fresh);
    rmap.store(fresh, std::memory_order_release);
    return fresh;
  }

  void file_changed() {
    read_generation.fetch_add(1, std::memory_order_release);
  }

public:
  bool is_dirty() {
    return dirty == 1;
  }

private:

  /* Index locking */ 
  void unlock() { 
    //bitmap_dbug("unlock");
    if(have_lock == LOCK_UN) return;
    flock(fileno(fp), LOCK_UN); 
    have_lock = LOCK_UN; 
  }

  void lock(int mode) {
    //bitmap_dbug("lock");
    if(have_lock == LOCK_EX || have_lock == mode) return;
    flock(fileno(fp), mode);
    have_lock = mode;
  }

  /* bits at current filepointer location */
  unsigned long long bits; 

  /* File pointers for data and log */
  FILE *fp=NULL; 
  FILE *log=NULL; 

  /* type of rwlock held by index */
  int lock_type = LOCK_UN; 

  /* filenames of the index and the log */
  std::string fname;
  std::string lname;

  /* flag to indicate that recovery is going on */
  int recovering = 0; 

  /* check for file existance */
  bool exists(std::string filename) {
    //bitmap_dbug("exists");
    struct stat buf;
    return !stat(filename.c_str(), &buf);
  }

  /* replay the changes in the log, in either redo(MODE_SET) or undo(MODE_UNSET) */
  int replay (int mode=MODE_UNSET) {
    //bitmap_dbug("replay");
    fseek(log,0,SEEK_SET);
    clearerr(log);
    fpos = 0;
    unsigned long long bitnum;
    while(!feof(log)) {
      bitnum = 0;
      int sz = fread(&bitnum,1,sizeof(bitnum), log);
      if(sz == 0) break;
      // bitnum 0 marks a commit
      if(bitnum) {
        set_bit(bitnum, mode);
      }
    }
    fsync(fileno(fp));
    
    return 0;
  }

  /* detect the commit marker ($) at the end of the file.
   * if the marker is found, then replay all the changes
   * in the log, marking the entries in the log as set.
   * otherwise, roll the changes back, unsetting the bits
   *
   * return: 
   * -1 - error
   *  0 - no recovery needed
   *  1 - recovery completed
   */ 
  int do_recovery() {
    //bitmap_dbug("do_recovery");
    recovering = 0;
    struct stat buf;
    log = NULL;
    //int orig_lock = lock_type;

    /* no lock has to be taken here */
    /* if log does not exist, no recovery needed*/
    int exists = !stat(lname.c_str(), &buf);
    if(!exists) {
      dirty = 0;
      return 0;
    } 

    /* lock the index */
    lock(LOCK_EX);

    /* avoid race: someone else may have recovered the log
     * if the log does not exist after obtaining the write
     * lock 
     */
    exists = !stat(lname.c_str(), &buf);
    if(!exists) {
      dirty = 0;
      return 0;
    } 

    recovering = 1;

    /* open the log if it is not open */
    if(!log) {
      log = fopen(lname.c_str(), "rb+");
    }

    if(!log) { 
      recovering = 0; 
      dirty=0;
      throw(1); 
    }
    //bitmap_dbug("STARTING RECOVERY");
    // start recovery
    int mode=MODE_SET;
    fseek(log,-BLOCK_SIZE,SEEK_END);
    unsigned long long marker;
    fread(&marker, BLOCK_SIZE, 1, log);
    if(marker!=0) {
      mode = MODE_UNSET;
    }
    fseek(log,0,SEEK_SET);

    // roll forward or roll back the changes
    int res = replay(mode);
    if(res) {
      fclose(log);
      throw(res);
    }

    close(1);
    recovering = 0;
    dirty = 0;
    return 1;
  }

public:
	sparsebitmap(std::string filename,int lock_mode = LOCK_SH) {
    //bitmap_dbug("construct");
    fp = NULL; 
    log = NULL;
    bits = 0;
    int open_state = open(filename, lock_mode);
    if(open_state != 0) {
      throw(open_state);
    }
  }

	~sparsebitmap() {
    for(auto &m : rmap_mappings) munmap(m.first, m.second);
    for(auto *m : rmap_all) delete m;
    unlock();
    if(fp){ fsync(fileno(fp)); fclose(fp); }
    if(log) { fsync(fileno(log)); fclose(log); }
  };

  std::string get_fname() {
    return fname;
  }

  /* -1 means already open */
  /* -2 means could not create*/
  /* -3 means could not open*/
  int open(std::string filename, int lock_mode = LOCK_SH) {
    //bitmap_dbug("open");
    if(fp != NULL) close();
    bits = 0;
    int skip_recovery = 0;
    
    fname = filename;
    lname = filename + ".txlog";

    reopen:
    /* open and/or create the file */
    fp = fopen(filename.c_str(),"rb+");
    if(!fp) { 
      fp = fopen(filename.c_str(),"wb");
      unlink(lname.c_str());
    }
    if(!fp) return -2;    
    fclose(fp);
    fp = fopen(filename.c_str(),"rb+");
    if(!fp) return -3;    

    /* if commit marker is in log, replay the log, otherwise
     * undo changes from the log
     */
    if(!skip_recovery && do_recovery() == 1) {
      skip_recovery = 1; 
      goto reopen;
    }
   
    lock(lock_mode);

    /* open the log */ 
    if(lock_mode == LOCK_EX) {
      log = fopen(lname.c_str(),"wb");
      if(!log) {
        return -4;
      }
    }

    /* read in the first block of bits */
    bits = 0;
    fread(&bits, BLOCK_SIZE, 1, fp);
    fseek(fp, 0, SEEK_SET);
        
    fpos = 0;
    dirty = 0;
    //bitmap_dbug("bits at open: " + std::to_string(bits));
    file_changed();

    return 0;
  }

  /* close the index */
  int close(int unlink_log = 0) {
    //bitmap_dbug("close");
    /* this will UNDO all the changes made to the index because commit() was not called*/
    //bitmap_dbug("dirty flag: " + std::to_string(dirty));
    if(!recovering && dirty) do_recovery();

    if(fp) fsync(fileno(fp));
    if(log) fsync(fileno(log));
    if(unlink_log) unlink(lname.c_str());
    
    if(fp) fclose(fp);
    if(log) fclose(log);
    unlock();
    log = NULL;
    fp = NULL;
    file_changed();

    /* release the lock held on the index*/

    return 0;
  } 
  /* Find the highest bit set in the bitmap. 
     returns 0 if a bit was found
     returns -1 if no bits were found
     returns -2 on error

     Note: Unless bits are explicitly set to zero in the bitmap,
     this should always read only the last BLOCK_SIZE (8 bytes)
     from the file.
  */
  int get_last_set_bit(uint64_t &last_bit) {
    if(!fp) {
      open(fname, LOCK_SH);
    } else {
      lock(LOCK_SH);
    }
    if(!fp) {
      return -2;
    }
    last_bit = 0;
    if(fseek(fp, 0, SEEK_END) != 0) return -2;
    long fsize = ftell(fp);
    if(fsize < 0) return -2;
    /* walk the blocks backwards, starting at the last complete block */
    for(long long block = (fsize / BLOCK_SIZE) - 1; block >= 0; --block) {
      fpos = block * BLOCK_SIZE;
      if(fseek(fp, fpos, SEEK_SET) != 0) return -2;
      bits = 0;
      if(fread(&bits, BLOCK_SIZE, 1, fp) != 1) return -2;
      if(!bits) continue;
      /* offset 0 holds the last bit of the block (bit 64), then 63..1 */
      if(bits & 1) {
        last_bit = (block * MAX_BITS) + MAX_BITS;
        return 0;
      }
      for(int bit_offset = MAX_BITS - 1; bit_offset > 0; --bit_offset) {
        if((bits >> bit_offset) & 1) {
          last_bit = (block * MAX_BITS) + bit_offset;
          return 0;
        }
      }
    }

    /* no bits are set */
    last_bit = 0;
    return -1; 
  }

  /* Find the last bit in the last black of bitmap. 
     Note: if this bit is zero, it may not have been
     explicity set to 0!
     returns -2 on error
  */
 int get_last_bit(uint64_t &last_bit) {
    if(!fp) {
      open(fname, LOCK_SH);
    } else {
      lock(LOCK_SH);
    }
    if(!fp) {
      return -2;
    }
    last_bit = 0;
    //assert(fseek(fp, 0, SEEK_END) == 0);
    struct stat st;
    if(stat(fname.c_str(), &st)) return 0;
    //fpos = ftell(fp);
    fprintf(stderr,"FPOS Is %s\n", std::to_string(st.st_size).c_str());
    fflush(stderr);
    last_bit = (BLOCK_SIZE * st.st_size) + (BLOCK_SIZE * 8);
    return 0;
  }

  /* sync the changes to disk, first the log
   * and then the data file */
  int commit() {
    //bitmap_dbug("commit");
    if(dirty) {
      //bitmap_dbug("bitmap is dirty - writing commit marker");
      //bitmap_dbug(fname.c_str());
      /* the commit marker is a whole block of zero bytes, the variable
         has to be as large as the block that is written */
      unsigned long long zero=0;
      static_assert(sizeof(zero) == BLOCK_SIZE, "commit marker size");
      int sz = fwrite(&zero, BLOCK_SIZE, 1, log);
      fsync(fileno(log));
      fsync(fileno(fp));
      dirty = 0;
      close(1);
      return !(sz == 1);
    }
    return 0;
  }

  int rollback() {
    close(1);
    return 0;
  }


  /* check to see if a particular bit is set.  This is called for every row
     of a scan, so it reads from the memory map of the file (see read_map)
     and does not take locks or make system calls. */
  inline int is_set(unsigned long long bitnum) {
    assert(bitnum > 0);
    const read_map *m = rmap.load(std::memory_order_acquire);
    if(m == NULL ||
       m->generation != read_generation.load(std::memory_order_acquire)) {
      m = refresh_read_map();
    }
    if(rmap_failed) {
      return is_set_file(bitnum);
    }
    int bit_offset;
    unsigned long long at_byte = ((bitnum / MAX_BITS) + ((bit_offset = (bitnum % MAX_BITS)) != 0) - 1) * BLOCK_SIZE;
    if(at_byte + BLOCK_SIZE > m->len) {
      return 0; /* past the end of the file, nothing was ever set there */
    }
    const uint64_t word =
        __atomic_load_n(&m->base[at_byte / BLOCK_SIZE], __ATOMIC_RELAXED);
    return (word >> bit_offset) & 1;
  }

  /* True if the file may have a bit set (it is not empty).  A scan does not
     have to look at the bitmap for every row if this is false. */
  inline bool may_have_bits() {
    const read_map *m = rmap.load(std::memory_order_acquire);
    if(m == NULL ||
       m->generation != read_generation.load(std::memory_order_acquire)) {
      m = refresh_read_map();
    }
    return rmap_failed || m->len > 0;
  }

  /* The way bits were read before the memory map, used if the file can not
     be mapped.  The FILE and the cached block are shared, so one thread at a
     time. */
  int is_set_file(unsigned long long bitnum) {
    std::lock_guard<std::mutex> guard(slow_mtx);
    if(!fp) open(fname, LOCK_SH);
    lock(LOCK_SH);
    
    int bit_offset;
    unsigned long long at_byte = ((bitnum / MAX_BITS) + ((bit_offset = (bitnum % MAX_BITS)) != 0) - 1) * BLOCK_SIZE;
    if(at_byte != fpos) {
      fseek(fp, at_byte, SEEK_SET);
      fpos = at_byte;
      bits = 0;
      size_t sz = fread(&bits, BLOCK_SIZE, 1, fp);
      
      if(sz == 0 || feof(fp)) { 
        bits = 0;
        return 0;
      }
    }
    int retval = (bits >> bit_offset) & 1; 
    return retval ;
  }

  /* set a bit in the index */
  /*  0 = successful write */
  /* -1 = logging failure (no change to index) */
  /* -2 = index read/write failure  */
  inline int set_bit(unsigned long long bitnum, int mode = MODE_SET) {
    //bitmap_dbug("set_bit");
    assert(bitnum > 0);
    if(!fp || have_lock != LOCK_EX) open(fname, LOCK_EX);
      
    /*
    bool force_read = false;
    if(dirty == 0) {
      force_read = true;
    } 
    */
    dirty = 1;
    // write-ahead-log (only write when not in recovery mode) 
    // sz2 will be <1 on write error
    size_t sz=0;
    if(!recovering) {
      uint64_t log_bitnum = bitnum;
      sz = fwrite(&log_bitnum, BLOCK_SIZE, 1, log);
      if(sz != 1) return -1;
    }

    if(lock_type != LOCK_EX) lock(LOCK_EX);

    /* which bit in the unsigned long long is to be set */
    int bit_offset;

    /* where to read at in file */
    unsigned long long at_byte = ((bitnum / MAX_BITS) + ((bit_offset = (bitnum % MAX_BITS)) != 0) - 1) * BLOCK_SIZE;

    /* read the bits into memory if necessary */
    if(at_byte != fpos) {
      fpos = at_byte;
      fseek(fp, at_byte, SEEK_SET);
      bits = 0;
      sz = fread(&bits, BLOCK_SIZE, 1, fp);
      if(ferror(fp)) return -2;
      if(sz == 0 || feof(fp)) bits = 0;
    }
    
    if(mode == MODE_SET)
      bits |= 1ULL << bit_offset; 
    else
      bits &= ~(1ULL << bit_offset);

    /* always seek: the file position may differ from fpos after a read */
    fseek(fp, at_byte, SEEK_SET);
    fpos = at_byte;

    sz = fwrite(&bits, BLOCK_SIZE, 1, fp);
    /* position back so that we don't read the block in again*/
    fseek(fp, at_byte, SEEK_SET); 
    return sz == 1 ? 0 : -2;
  }

  /*
  // todo: support numbered savepoints
  int create_savepoint(uint64_t savepoint_num) {
    //bitmap_dbug("create_savepoint");
    if(!fp || have_lock != LOCK_EX) open(fname, LOCK_EX);
    std::string savepoint_fname = std::string("sp_") + fname + std::string("_") + std::to_string(savepoint_num) + std::string(".txlog");
    FILE* splog = fopen(savepoint_fname.c_str, "wb+");
    if(splog == NULL) {
      sql_print_error("Could not open savepoint log: %s" + splog_fname.c_str());
      assert(false);
    }
    return 0;
  }

  // todo: support numbered savepoints
  int rollback_to_savepoint(uint64_t savepoint_num) {
    //bitmap_dbug("rollback_to_savepoint");

    clearerr(log);
    fseek(log, 0, SEEK_SET);
    uint64_t log_fpos = 0;
    unsigned long long bitnum;
    do {
      bitnum = 0;
      int sz = fread(&bitnum,1,BLOCK_SIZE, log);
      log_fpos = ftell(log);
      if(sz == 0) break;
    } while(bitnum != 2);
    if(feof(log)) {
      // savepoint not found
      return 0;
    }
    uint64_t savepoint_pos = log_fpos - BLOCK_SIZE;
    while(true) {
      int sz = fread(&bitnum,1,BLOCK_SIZE, log);
      if(sz == 0) {
        break;
      }
      set_bit(bitnum, MODE_UNSET);
    }
    ftruncate(fileno(log), savepoint_pos);
    fsync(fileno(log));
    fsync(fileno(fp));
    return 0;
  }
*/
};

#endif
