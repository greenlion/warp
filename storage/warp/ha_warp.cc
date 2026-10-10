/* Copyright (c) 2020 Justin Swanhart

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA 

  ------
  
  This software uses MySQL which is also available under the above GPL2 
  license.
  
  ------

  This software uses Fastbit:
  "FastBit, Copyright (c) 2014, The Regents of the University of
  California, through Lawrence Berkeley National Laboratory (subject to
  receipt of any required approvals from the U.S. Dept. of Energy).  All
  rights reserved."

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  (1) Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  (2) Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  (3) Neither the name of the University of California, Lawrence Berkeley
  National Laboratory, U.S. Dept. of Energy nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  You are under no obligation whatsoever to provide any bug fixes,
  patches, or upgrades to the features, functionality or performance of
  the source code ("Enhancements") to anyone; however, if you choose to
  make your Enhancements available either publicly, or directly to
  Lawrence Berkeley National Laboratory, without imposing a separate
  written license agreement for such Enhancements, then you hereby grant
  the following license: a non-exclusive, royalty-free perpetual license
  to install, use, modify, prepare derivative works, incorporate into
  other computer software, distribute, and sublicense such enhancements or
  derivative works thereof, in binary and source code form.
*/
#define WARP_BITMAP_DEBUG
#include <errno.h>
#include <optional>
#include "ha_warp.h"
#include "sql/tztime.h"
#ifdef WARP_USE_SIMD_INTERSECTION
#include "include/lemire-sorted-simd/codecfactory.h"
#include "include/lemire-sorted-simd/intersection.h"
using namespace SIMDCompressionLib;
#endif

int warp_push_to_engine(THD *, AccessPath *, JOIN *);
// Stuff for shares */
mysql_mutex_t warp_mutex;
static std::unique_ptr<collation_unordered_multimap<std::string, WARP_SHARE *>>
    warp_open_tables;

static handler *warp_create_handler(handlerton *hton, TABLE_SHARE *table,
                                    bool partitioned, MEM_ROOT *mem_root);

static handler *warp_create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                                    MEM_ROOT *mem_root) {
  return new (mem_root) ha_warp(hton, table);
}

/* TIME values are stored as signed microseconds biased by WARP_TIME_BIAS so
   that they fit in an unsigned Fastbit column and still sort correctly.
   The bias is larger than the TIME range (+/- 838:59:59.999999).
   All other temporal types are stored as packed DATETIME values.
*/
static constexpr uint64_t WARP_TIME_BIAS = 1ULL << 42;

static inline bool warp_is_time_type(enum_field_types type) {
  return type == MYSQL_TYPE_TIME || type == MYSQL_TYPE_TIME2;
}

static inline uint64_t warp_encode_time(const Time_val &time) {
  return static_cast<uint64_t>(time.to_microseconds()) + WARP_TIME_BIAS;
}

static inline Time_val warp_decode_time(uint64_t value) {
  const int64_t usec = static_cast<int64_t>(value - WARP_TIME_BIAS);
  const bool negative = usec < 0;
  const uint64_t abs_usec = negative ? -static_cast<uint64_t>(usec) : usec;
  if (abs_usec == 0) return Time_val(false, 0, 0);
  return Time_val(negative, static_cast<uint32_t>(abs_usec / 1000000),
                  static_cast<uint32_t>(abs_usec % 1000000));
}

/* Evaluate a temporal Item into the same encoding used for storage */
static uint64_t warp_item_temporal_value(Item *item) {
  if (warp_is_time_type(item->data_type())) {
    Time_val time;
    if (item->val_time(&time)) return 0;
    return warp_encode_time(time);
  }
  return item->val_date_temporal();
}

/* TIMESTAMP values are stored in UTC, as packed DATETIME values, like
   MySQL stores them.  A TIMESTAMP that is zero ('0000-00-00 00:00:00') is
   stored as 0. */
static inline bool warp_is_timestamp_type(enum_field_types type) {
  return type == MYSQL_TYPE_TIMESTAMP || type == MYSQL_TYPE_TIMESTAMP2;
}

static uint64_t warp_pack_utc(const my_timeval &tv) {
  if (tv.m_tv_sec == 0 && tv.m_tv_usec == 0) return 0;
  MYSQL_TIME utc;
  my_tz_UTC->gmt_sec_to_TIME(&utc, tv);
  return TIME_to_longlong_datetime_packed(utc);
}

static void warp_unpack_utc(uint64_t value, my_timeval *tv) {
  tv->m_tv_sec = 0;
  tv->m_tv_usec = 0;
  if (value == 0) return;
  MYSQL_TIME utc;
  TIME_from_longlong_datetime_packed(&utc, value);
  bool in_gap = false;
  tv->m_tv_sec = my_tz_UTC->TIME_to_gmt_sec(&utc, &in_gap);
  tv->m_tv_usec = utc.second_part;
}

/* The stored value of a TIMESTAMP field */
static uint64_t warp_timestamp_field_value(const Field *field) {
  my_timeval tv;
  int warnings = 0;
  if (field->get_timestamp(&tv, &warnings)) return 0;
  return warp_pack_utc(tv);
}

/* The stored value that a temporal Item is compared with a TIMESTAMP column
   as: the value of the Item is a time in the time zone of the session.
   Returns false if the value is not a TIMESTAMP (it is out of the range). */
static bool warp_item_timestamp_value(Item *item, THD *thd, uint64_t &out) {
  const uint64_t local = item->val_date_temporal();
  if (item->null_value) return false;
  if (local == 0) {
    out = 0;
    return true;
  }
  MYSQL_TIME lt;
  TIME_from_longlong_datetime_packed(&lt, local);
  bool in_gap = false;
  const my_time_t sec = thd->time_zone()->TIME_to_gmt_sec(&lt, &in_gap);
  if (sec <= 0 || sec > 2147483647) return false;
  my_timeval tv;
  tv.m_tv_sec = sec;
  tv.m_tv_usec = lt.second_part;
  out = warp_pack_utc(tv);
  return true;
}

/*****************************************************************************
 ** WARP tables
 *****************************************************************************/
#ifdef HAVE_PSI_INTERFACE
static PSI_memory_key warp_key_memory_warp_share;
static PSI_memory_key warp_key_memory_row;
static PSI_memory_key warp_key_memory_blobroot;

static PSI_mutex_key warp_key_mutex_warp, warp_key_mutex_WARP_SHARE_mutex;

static PSI_mutex_info all_warp_mutexes[] = {
    {&warp_key_mutex_warp, "warp", PSI_FLAG_SINGLETON, 0, PSI_DOCUMENT_ME},
    {&warp_key_mutex_WARP_SHARE_mutex, "WARP_SHARE::mutex", 0, 0,
     PSI_DOCUMENT_ME}};

static PSI_memory_info all_warp_memory[] = {
    {&warp_key_memory_warp_share, "WARP_SHARE", PSI_FLAG_ONLY_GLOBAL_STAT, 0,
     PSI_DOCUMENT_ME},
    {&warp_key_memory_blobroot, "blobroot", 0, 0, PSI_DOCUMENT_ME},
    {&warp_key_memory_row, "row", 0, 0, PSI_DOCUMENT_ME}};

/*
static PSI_file_key warp_key_file_metadata, warp_key_file_data,
    warp_key_file_update;
*/

static void init_warp_psi_keys(void) {
  const char *category = "warp";
  int count;

  count = static_cast<int>(array_elements(all_warp_mutexes));
  mysql_mutex_register(category, all_warp_mutexes, count);

  count = static_cast<int>(array_elements(all_warp_memory));
  mysql_memory_register(category, all_warp_memory, count);
}
#endif /* HAVE_PSI_INTERFACE */

struct st_mysql_storage_engine warp_storage_engine = { MYSQL_HANDLERTON_INTERFACE_VERSION };
static int warp_init_func(void *p);
static int warp_done_func(void *p);

/* The data files of a table: readers and writers.

   A writer appends rows to the column files of a partition one column after
   the other and updates the row count of the partition.  A scan that opens
   the partition at that moment sees a row count that the column files do not
   have yet (bord::append could only add 8005 of 8006 values, error -19) and
   fails.  Scans and the other code that read the data files hold the lock of
   the table shared while they do that, the writer holds it exclusive.  The
   rows that a scan returns are in memory (a table that select made), so
   the lock is not held while they are returned.

   The locks are kept by the name of the data directory of the table, so
   code that only has the name (the dimension table of a join) finds the same
   lock.  A thread that holds a lock for reading does not take it again. */
static std::shared_ptr<std::shared_mutex> warp_table_lock(const char *data_dir) {
  static std::mutex registry_mtx;
  static std::unordered_map<std::string, std::shared_ptr<std::shared_mutex>> registry;
  std::lock_guard<std::mutex> guard(registry_mtx);
  auto it = registry.find(data_dir);
  if(it == registry.end()) {
    it = registry.emplace(std::string(data_dir), std::make_shared<std::shared_mutex>()).first;
  }
  return it->second;
}

static thread_local std::unordered_map<const std::shared_mutex *, int> warp_read_locks_held;

class warp_table_read_lock {
 public:
  explicit warp_table_read_lock(const char *data_dir)
      : lock_(warp_table_lock(data_dir)) {
    if(warp_read_locks_held[lock_.get()]++ == 0) {
      lock_->lock_shared();
    }
  }
  ~warp_table_read_lock() {
    auto it = warp_read_locks_held.find(lock_.get());
    if(--(it->second) == 0) {
      warp_read_locks_held.erase(it);
      lock_->unlock_shared();
    }
  }
  warp_table_read_lock(const warp_table_read_lock &) = delete;
  warp_table_read_lock &operator=(const warp_table_read_lock &) = delete;

 private:
  std::shared_ptr<std::shared_mutex> lock_;
};

class warp_table_write_lock {
 public:
  explicit warp_table_write_lock(const char *data_dir)
      : lock_(warp_table_lock(data_dir)) {
    /* a thread that reads can not wait for the writers, it would wait
       for itself */
    if(warp_read_locks_held.find(lock_.get()) == warp_read_locks_held.end()) {
      lock_->lock();
      locked_ = true;
    } else {
      sql_print_warning("WARP: rows are written by a thread that is reading %s", data_dir);
    }
  }
  ~warp_table_write_lock() {
    if(locked_) lock_->unlock();
  }
  warp_table_write_lock(const warp_table_write_lock &) = delete;
  warp_table_write_lock &operator=(const warp_table_write_lock &) = delete;

 private:
  std::shared_ptr<std::shared_mutex> lock_;
  bool locked_ = false;
};

/* Status variables: Warp_history_locks is the number of history locks that
   exist (see warp_global_data::cleanup_history_locks), Warp_active_transactions
   the number of transactions that exist (read only ones too) */
static int show_warp_history_locks(MYSQL_THD, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *reinterpret_cast<longlong *>(buff) =
      (warp_state != NULL ? (longlong)warp_state->history_lock_total() : 0);
  return 0;
}

static int show_warp_active_transactions(MYSQL_THD, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *reinterpret_cast<longlong *>(buff) =
      (warp_state != NULL ? (longlong)warp_state->active_trx_total() : 0);
  return 0;
}

static SHOW_VAR warp_status_variables[] = {
    {"Warp_history_locks", (char *)&show_warp_history_locks, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_active_transactions", (char *)&show_warp_active_transactions, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

mysql_declare_plugin(warp){
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &warp_storage_engine,
  "WARP",
  "Justin Swanhart",
  "WARP columnar storage engine(using FastBit 2.0.3 storage)",
  PLUGIN_LICENSE_GPL,
  warp_init_func, /* Plugin Init */
  NULL,           /* Plugin check uninstall */
  warp_done_func, /* Plugin Deinit */
  0x203 /* Based on Fastbit 2.0.3 */,
  warp_status_variables, /* status variables                */
  system_variables, /* system variables    */
  NULL,             /* config options                  */
  0,                /* flags                           */
} mysql_declare_plugin_end;

 
static int warp_init_func(void *p) {
  DBUG_ENTER("warp_init_func");
  sql_print_information("WARP storage engine initialization started");
  handlerton *warp_hton;
  if(my_cache_size>0) {
    if(ibis::fileManager::adjustCacheSize(my_cache_size) != 0) {
      /* FastBit refuses a size that is not larger than the memory it is
         already using, and keeps its default size */
      sql_print_warning("WARP: warp_cache_size = %llu is too small and was ignored, "
                        "the FastBit cache size is %llu bytes",
                        (unsigned long long)my_cache_size,
                        (unsigned long long)ibis::fileManager::currentCacheSize());
    }
  }

  ibis::init(NULL, "/tmp/fastbit.log");
  ibis::util::setVerboseLevel(0);
  warp_apply_compression();
#ifdef HAVE_PSI_INTERFACE
  init_warp_psi_keys();
#endif
  
  warp_hton = (handlerton *)p;
  mysql_mutex_init(warp_key_mutex_warp, &warp_mutex, MY_MUTEX_INIT_FAST);
  warp_open_tables.reset(
      new collation_unordered_multimap<std::string, WARP_SHARE *>(
          system_charset_info, warp_key_memory_warp_share));
  warp_hton->state = SHOW_OPTION_YES;
  warp_hton->db_type = DB_TYPE_UNKNOWN;
  warp_hton->create = warp_create_handler;
  warp_hton->flags = (HTON_CAN_RECREATE | HTON_NO_PARTITION);
  warp_hton->file_extensions = ha_warp_exts;
  warp_hton->rm_tmp_tables = default_rm_tmp_tables;
  warp_hton->commit = warp_commit;
  warp_hton->rollback = warp_rollback;
  warp_hton->push_to_engine = warp_push_to_engine;
  
  // starts the database and reads in the database state, upgrades
  // tables and does crash recovery
  warp_state = new warp_global_data();
  
  assert(warp_state != NULL);
  sql_print_information("WARP storage engine initialization completed");
  DBUG_RETURN(0);
}

static int warp_done_func(void *) {
  sql_print_information("WARP storage engine shutdown started");
  warp_open_tables.reset();

  // destroying warp_state writes the state to disk
  delete warp_state;
  mysql_mutex_destroy(&warp_mutex);
  sql_print_information("WARP storage engine shutdown completed");
  return 0;
}

/* Construct the warp handler */
ha_warp::ha_warp(handlerton *hton, TABLE_SHARE *table_arg) 
: handler(hton, table_arg),
  base_table(NULL),
  filtered_table(NULL),
  cursor(NULL),
  writer(NULL),
  current_rowid(0),
  blobroot(warp_key_memory_blobroot, BLOB_MEMROOT_ALLOC_SIZE) 
{
  warp_hton = hton;
}

const char **ha_warp::bas_ext() const {  
  return ha_warp_exts;
}

int ha_warp::rename_table(const char * from, const char * to, const dd::Table* , dd::Table* ) {
  DBUG_ENTER("ha_example::rename_table ");
  /* The FastBit cache is keyed by file name.  The files of both names are
     dropped from it, otherwise a table that is created later with one of the
     names would read the content of the old files. */
  const std::string from_dir = std::string(from) + ".data";
  const std::string to_dir = std::string(to) + ".data";
  ibis::fileManager::instance().flushDir(from_dir.c_str());
  ibis::fileManager::instance().flushDir(to_dir.c_str());
  std::string cmd = "mv " + std::string(from) + ".data/ " + std::string(to) + ".data/";
  
  __attribute__((unused))int retval = system(cmd.c_str()); 
  ibis::fileManager::instance().flushDir(from_dir.c_str());
  ibis::fileManager::instance().flushDir(to_dir.c_str());
  DBUG_RETURN(0);
}

bool ha_warp::is_deleted(uint64_t rownum) {
  return warp_state->delete_bitmap->is_set(rownum);
}

/*
void ha_warp::get_auto_increment(ulonglong, ulonglong, ulonglong,
                                 ulonglong *first_value,
                                 ulonglong *nb_reserved_values) {
  *first_value = stats.auto_increment_value ? stats.auto_increment_value : 1;
  *nb_reserved_values = ULLONG_MAX;
}
*/

int ha_warp::encode_quote(uchar *) {
  char attribute_buffer[1024];
  String attribute(attribute_buffer, sizeof(attribute_buffer), &my_charset_bin);
  buffer.length(0);

  for (Field **field = table->field; *field; field++) {
    const char *ptr;
    const char *end_ptr;

    /* For both strings and numeric types, the value of a NULL
       column in the database is 0. This value isn't ever used
       as it is just a placeholder. The associated NULL marker
       is marked as 1.  There are no NULL markers for columns
       which are NOT NULLable.

       This side effect must be handled by condition pushdown
       because comparisons for the value zero must take into
       account the NULL marker and it is also used to handle
       IS NULL/IS NOT NULL too.
    */
    if((*field)->is_null()) {
      buffer.append("0,1,");
      continue;
    }

    /* Convert the value to string */
    bool no_quote = false;
    attribute.length(0);
    switch((*field)->real_type()) {
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
        (*field)->val_str(&attribute, &attribute);
        break;

      case MYSQL_TYPE_YEAR:
        (*field)->val_int_as_str(&attribute, false);

        no_quote = true;
        break;

      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIME2: 
      {
        if (warp_is_time_type((*field)->real_type())) {
          Time_val tmp_time;
          (*field)->val_time(&tmp_time);
          attribute.append(std::to_string(warp_encode_time(tmp_time)).c_str());
        } else if (warp_is_timestamp_type((*field)->real_type())) {
          attribute.append(
              std::to_string(warp_timestamp_field_value(*field)).c_str());
        } else {
          Datetime_val tmp_dt;
          (*field)->val_datetime(&tmp_dt, TIME_DATETIME_ONLY);
          auto tmp = TIME_to_longlong_datetime_packed(tmp_dt);
          attribute.append(std::to_string(tmp).c_str());
        }
        no_quote = true;
      }

      break;

      default:
        (*field)->val_str(&attribute, &attribute);
        break;
    }

    /* MySQL is going to tell us that the date and time types need quotes
       in string form, but they are being written into the storage engine
       in integer format the quotes are not needed in this encapsulation.
    */
    if((*field)->str_needs_quotes() && !no_quote) {
      ptr = attribute.ptr();
      end_ptr = attribute.length() + ptr;

      buffer.append('"');

      /* FastBit's parser (ibis::util::readString) only understands a
         backslash in front of the quote character or another backslash.
         Every other character, including newlines, is copied as is. */
      for (; ptr < end_ptr; ptr++) {
        if(*ptr == '"' || *ptr == '\\') {
          buffer.append('\\');
          buffer.append(*ptr);
        } else if(*ptr == 0) {
          /* FastBit stores strings null terminated, so an embedded null
             can not be stored */
          buffer.append('\\');
          buffer.append('0');
        } else {
          buffer.append(*ptr);
        }
      }
      buffer.append('"');
    } else {
      buffer.append(attribute);
    }

    /* A NULL marker (for example the column n0 for column c0) is
       marked as zero when the value is not NULL. The NULL marker
       column is always included in a fetch for the corresponding
       cX column. NOT NULL columns do not have an associated NULL
       marker.  Note the trailing comma (also above).
    */
    if((*field)->is_nullable()) {
      buffer.append(",0,");
    } else {
      buffer.append(',');
    }
  }

  /* the RID column is at the end of every table */
  buffer.append(std::to_string(current_rowid).c_str());
    
  /* add the transaction identifier */
  auto current_trx=warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  buffer.append(",");
  buffer.append(std::to_string(current_trx->trx_id).c_str());
  return (buffer.length());
}

/*
  Simple lock controls.
*/
static WARP_SHARE *get_share(const char *table_name, TABLE *) {
  DBUG_ENTER("ha_warp::get_share");
  WARP_SHARE *share;
  char *tmp_name;
  uint length;
  length = (uint)strlen(table_name);

  mysql_mutex_lock(&warp_mutex);

  /*
    If share is not present in the hash, create a new share and
    initialize its members.
  */
  const auto it = warp_open_tables->find(table_name);
  if(it == warp_open_tables->end()) {
    if(!my_multi_malloc(warp_key_memory_warp_share, MYF(MY_WME | MY_ZEROFILL),
                         &share, sizeof(*share), &tmp_name, length + 1,
                         NullS)) {
      mysql_mutex_unlock(&warp_mutex);
      return NULL;
    }

    share->use_count = 0;
    share->table_name.assign(table_name, length);
    /* This is where the WARP data is actually stored.  It is usually 
       something like /var/lib/mysql/dbname/tablename.data
    */
    fn_format(share->data_dir_name, table_name, "", ".data",
              MY_REPLACE_EXT | MY_UNPACK_FILENAME);

    warp_open_tables->emplace(table_name, share);
    thr_lock_init(&share->lock);
    mysql_mutex_init(warp_key_mutex_WARP_SHARE_mutex, &share->mutex,
                     MY_MUTEX_INIT_FAST);

  } else {
    share = it->second;
  }

  share->use_count++;
  mysql_mutex_unlock(&warp_mutex);

  DBUG_RETURN(share);
}

bool ha_warp::check_and_repair(THD *) {
  HA_CHECK_OPT check_opt;
  DBUG_ENTER("ha_warp::check_and_repair");
  /*
  check_opt.init();

  DBUG_RETURN(repair(thd, &check_opt));
  */
  DBUG_RETURN(-1);
}

bool ha_warp::is_crashed() const {
  DBUG_ENTER("ha_warp::is_crashed");
  DBUG_RETURN(0);
}

/*
  Free lock controls.
*/
static int free_share(WARP_SHARE *share) {
  DBUG_ENTER("ha_warp::free_share");
  mysql_mutex_lock(&warp_mutex);
  int result_code = 0;
  if(!--share->use_count) {
    warp_open_tables->erase(share->table_name.c_str());
    thr_lock_delete(&share->lock);
    mysql_mutex_destroy(&share->mutex);
    my_free(share);
  }
  mysql_mutex_unlock(&warp_mutex);
  
  DBUG_RETURN(result_code);
}


int ha_warp::set_column_set() {
  DBUG_ENTER("ha_warp::set_column_set");
  column_set = "";

  int count = 0;
  for (Field **field = table->field; *field; field++) {
    if(bitmap_is_set(table->read_set, (*field)->field_index()) || current_thd->lex->sql_command == SQLCOM_UPDATE || current_thd->lex->sql_command == SQLCOM_UPDATE_MULTI || current_thd->lex->sql_command == SQLCOM_DELETE || current_thd->lex->sql_command == SQLCOM_DELETE_MULTI ) {
      ++count;

      /* this column must be read from disk */
      column_set += std::string("c") + std::to_string((*field)->field_index());

      /* Add the NULL bitmap for the column if the column is NULLable */
      if((*field)->is_nullable()) {
        column_set +=
            "," + std::string("n") + std::to_string((*field)->field_index());
      }
      column_set += ",";
    }
  }

  /* The RID column (r) needs to be read always in order to support UPDATE and
     DELETE. For queries that neither SELECT nor PROJECT columns, the RID column
     will be projected regardless.  The RID column is never included in the
     result set.

     The TRX_ID column (t) must be read for transaction visibility and to 
     exclude rows that were not commited.
  */
  column_set += "r,t";

  count=0;

  DBUG_RETURN(count + 1);
}

/* store the binary data for each returned value into the MySQL buffer
   using field->store()
*/
int ha_warp::find_current_row(uchar *buf, ibis::table::cursor *cursor) {
  DBUG_ENTER("ha_warp::find_current_row");
  int rc = 0;
  memset(buf, 0, table->s->null_bytes);
  
  // Clear BLOB data from the previous row.
  blobroot.ClearForReuse();

  /* Avoid asserts in ::store() for columns that are not going to be updated */
  my_bitmap_map *org_bitmap(dbug_tmp_use_all_columns(table, table->write_set));
  
  /* Read all columns when a table is opened for update */
  

  for (Field **field = table->field; *field; field++) {
    buffer.length(0);
    if(bitmap_is_set(table->read_set, (*field)->field_index()) || current_thd->lex->sql_command == SQLCOM_UPDATE ||  current_thd->lex->sql_command == SQLCOM_UPDATE_MULTI  || current_thd->lex->sql_command == SQLCOM_DELETE || current_thd->lex->sql_command == SQLCOM_DELETE_MULTI ) {
      
      bool is_unsigned = (*field)->all_flags() & UNSIGNED_FLAG;
      std::string cname = "c" + std::to_string((*field)->field_index());
      std::string nname = "n" + std::to_string((*field)->field_index());

      if((*field)->is_nullable()) {
        unsigned char is_null = 0;

        rc = cursor->getColumnAsUByte(nname.c_str(), is_null);

        /* This column value is NULL */
        if(is_null != 0) {
          (*field)->set_null();
          rc = 0;
          continue;
        }
      }

      switch((*field)->real_type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_YEAR: {
          if(is_unsigned) {
            unsigned int tmp = 0;
            rc = cursor->getColumnAsUInt(cname.c_str(), tmp);
            rc = (*field)->store(tmp, true);
          } else {
            int tmp = 0;
            rc = cursor->getColumnAsInt(cname.c_str(), tmp);
            rc = (*field)->store(tmp, false);
          }
          break;
        }
        case MYSQL_TYPE_SHORT: {
          if(is_unsigned) {
            uint16_t tmp = 0;
            rc = cursor->getColumnAsUShort(cname.c_str(), tmp);
            rc = (*field)->store(tmp, true);
          } else {
            int16_t tmp = 0;
            rc = cursor->getColumnAsShort(cname.c_str(), tmp);
            rc = (*field)->store(tmp, false);
          }
        } break;

        case MYSQL_TYPE_LONG: {
          if(is_unsigned) {
            uint32_t tmp = 0;
            rc = cursor->getColumnAsUInt(cname.c_str(), tmp);
            rc = (*field)->store(tmp, true);
          } else {
            int32_t tmp = 0;
            rc = cursor->getColumnAsInt(cname.c_str(), tmp);
            rc = (*field)->store(tmp, false);
          }
        } break;

        case MYSQL_TYPE_LONGLONG: {
          uint64_t tmp = 0;
          if(is_unsigned) {
            rc = cursor->getColumnAsULong(cname.c_str(), tmp);
            rc = (*field)->store(tmp, true);
          } else {
            int64_t tmp = 0;
            rc = cursor->getColumnAsLong(cname.c_str(), tmp);
            rc = (*field)->store(tmp, false);
          }
        } break;

        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_JSON: {
          std::string tmp;
          rc = cursor->getColumnAsString(cname.c_str(), tmp);
          if((*field)->store(tmp.c_str(), tmp.length(), (*field)->charset(),
                              CHECK_FIELD_WARN)) {
            rc = HA_ERR_CRASHED_ON_USAGE;
            goto err;
          }
          if((*field)->all_flags() & BLOB_FLAG) {
            Field_blob *blob_field = down_cast<Field_blob *>(*field);
            
            size_t length = blob_field->get_length();
            // BLOB data is not stored inside buffer. It only contains a
            // pointer to it. Copy the BLOB data into a separate memory
            // area so that it is not overwritten by subsequent calls to
            // Field::store() after moving the offset.
            if(length > 0) {
              const unsigned char *old_blob;
              old_blob = blob_field->data_ptr();
              unsigned char *new_blob = new (&blobroot) unsigned char[length];
              
              if(new_blob == nullptr) DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              memcpy(new_blob, old_blob, length);
              blob_field->set_ptr(length, new_blob);

            }
          }
        }

        break;

        case MYSQL_TYPE_FLOAT: {
          float_t tmp;
          rc = cursor->getColumnAsFloat(cname.c_str(), tmp);
          rc = (*field)->store(tmp);
        } break;

        case MYSQL_TYPE_DOUBLE: {
          double_t tmp;
          rc = cursor->getColumnAsDouble(cname.c_str(), tmp);
          rc = (*field)->store(tmp);
        } break;

        case MYSQL_TYPE_INT24: {
          uint32_t tmp;
          if(is_unsigned) {
            rc = cursor->getColumnAsUInt(cname.c_str(), tmp);
            rc = (*field)->store(tmp, true);
          } else {
            int32_t tmp;
            rc = cursor->getColumnAsInt(cname.c_str(), tmp);
            rc = (*field)->store(tmp, false);
          }
        } break;

        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
        case MYSQL_TYPE_DATETIME2: {
          
          uint64_t tmp;
          rc = cursor->getColumnAsULong(cname.c_str(), tmp);
          /* See encode_quote() for the on-disk temporal formats. Not
             every temporal Field implements store_packed() in 9.x, so
             unpack and store via store_time() instead. */
          if (warp_is_time_type((*field)->real_type())) {
            rc = (*field)->store_time(warp_decode_time(tmp),
                                      (*field)->decimals());
          } else if (warp_is_timestamp_type((*field)->real_type())) {
            my_timeval tv;
            warp_unpack_utc(tmp, &tv);
            (*field)->store_timestamp(&tv);
            rc = 0;
          } else {
            MYSQL_TIME ltime;
            TIME_from_longlong_datetime_packed(&ltime, tmp);
            rc = (*field)->store_time(&ltime, (*field)->decimals());
          }
        } break;
        /* the following are stored as strings in Fastbit */
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_NULL:
        case MYSQL_TYPE_BIT:
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
        case MYSQL_TYPE_GEOMETRY: {
          std::string tmp;
          rc = cursor->getColumnAsString(cname.c_str(), tmp);
          if((*field)->store(tmp.c_str(), tmp.length(), (*field)->charset(),
                              CHECK_FIELD_WARN)) {
            rc = HA_ERR_CRASHED_ON_USAGE;
            goto err;
          }
        } break;

        default: {
          std::string errmsg = "Unsupported data type for column: " +
                               std::string((*field)->field_name);
          my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), errmsg.c_str());
          rc = HA_ERR_UNSUPPORTED;
          goto err;
          break;
        }
      }

      if(rc != 0) {
        goto err;
      }
    }
  }

err:
  dbug_tmp_restore_column_map(table->write_set, org_bitmap);
  
  DBUG_RETURN(rc);
}

int ha_warp::reset_table() {
  DBUG_ENTER("ha_warp::reset_table");
  /* ECP is reset here */
  push_where_clause = "";

  DBUG_RETURN(0);
}

void ha_warp::update_row_count() {
  DBUG_ENTER("ha_warp::row_count");
  /* The table is only opened to count the rows.  It must not be kept: it
     holds the column files of the partitions in the FastBit cache, and as long
     as one connection with the table open does that, the files the writers
     append to can not be removed from the cache (they are "in use") and the
     scans that follow read the old copy with the new number of rows. */
  warp_table_read_lock data_lock(share->data_dir_name);
  std::unique_ptr<ibis::mensa> counted(new ibis::mensa(share->data_dir_name));
  stats.records = counted->nRows();
  DBUG_VOID_RETURN;
}

int ha_warp::open(const char *name, int, uint, const dd::Table *) {
  DBUG_ENTER("ha_warp::open");
  if(!(share = get_share(name, table))) DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  update_row_count();

  //  FIXME: support concurrent insert for LDI
  thr_lock_data_init(&share->lock, &lock, (void *)this);
  ref_length = sizeof(my_off_t);

  /* These closures are used to allow concurrent insert.  It isn't
     working with LOAD DATA INFILE though.  LDI sends 0 for the
     concurrent_insert parameter and requests a TL_WRITE lock.
     INSERT INTO ... however sends 1 and requests a
     TL_WRITE_CONCURRENT_INSERT lock and concurent insert works. I
     need to figure out how to get MySQL to allow concurrent
     insert for LDI
  */

  auto get_status = [](void *, int) { return; };

  auto update_status = [](void *) { return; };

  auto check_status = [](void *) { return false; };

  share->lock.get_status = get_status;
  share->lock.update_status = update_status;
  share->lock.check_status = check_status;

  /* reserve space for the buffer for INSERT statements */
  buffer.alloc(65535);

  DBUG_RETURN(0);
}

/*
  Close a database file. We remove ourselves from the shared strucutre.
  If it is empty we destroy it.
*/
int ha_warp::close(void) {
  DBUG_ENTER("ha_warp::close");
  if(index_scan_open) {
    end_scan(true);
  }
  discard_prefetched();
  if(writer) {
    writer->clearData();
    delete writer;
  } 
  writer = NULL;
  if(cursor) delete cursor;
  cursor = NULL;
  if(filtered_table) delete filtered_table;
  filtered_table = NULL;
  if(base_table) delete base_table;
  base_table = NULL;

  DBUG_RETURN(free_share(share));
}

void ha_warp::start_bulk_insert(ha_rows) {
}

int ha_warp::end_bulk_insert() {
  if(writer != NULL) {
    /* foreground write actually because it is not executed in a different `thread */
    return write_buffered_rows_to_disk();
  }

  return 0;
}

std::string ha_warp::get_writer_partition() {
  auto parts = new ibis::partList ;
  int partition_count = ibis::util::gatherParts(*parts, share->data_dir_name);
  std::string retval;
  write_mutex.lock();
  // if there is only one partition, the table is empty and a new p0 must be created
  if(partition_count == 1) {
    retval =  std::string(share->data_dir_name) + std::string("/p0");
    goto done;
  }
  
  for (auto it = parts->begin(); it < parts->end(); ++it) {
    // skip the top level partition
    if(std::string((*it)->currentDataDir()) == std::string(share->data_dir_name)) {
      continue;
    }
    // find the partition with the least number of rows (top level partition is excluded above)
    if(writer->mRows() + (*it)->nRows() <= my_partition_max_rows) {
      retval = std::string((*it)->currentDataDir());
      goto done;
    }
  }   
  
  retval = std::string(share->data_dir_name) + std::string("/p") + (std::to_string(parts->size()-1));
  done:
  write_mutex.unlock();
  
  for(auto it=parts->begin();it!=parts->end();++it) {
    delete *it;
    *it=NULL;
  }
  delete parts;
  return retval;
}

/* Write the buffered rows to disk and empty the buffer.  Returns 0 or a
   handler error code.  FastBit reports a lack of memory (typically a
   FastBit cache that is too small) by throwing exceptions, which must not
   reach the server. */
int ha_warp::write_buffered_rows_to_disk() {
  int rc = 0;
  mysql_mutex_lock(&share->mutex);

  try {
    /* no scan reads the partition while it is written */
    warp_table_write_lock data_lock(share->data_dir_name);
    std::string part_dir = get_writer_partition();
    std::string part_name = part_dir.substr(part_dir.find_last_of('/') + 1);
    int written = writer->write(part_dir.c_str(), part_name.c_str());
    if(written < 0) {
      sql_print_error("WARP: could not write the buffered rows to %s (error %d)",
                      part_dir.c_str(), written);
      rc = HA_ERR_INTERNAL_ERROR;
    }
    /* The rows were appended to the column files.  A copy of a file that is in
       the FastBit cache is shorter than the file now, and the scans that
       come later would read the old copy with the new row count. */
    ibis::fileManager::instance().flushDir(part_dir.c_str());
  } catch(...) {
    sql_print_error("WARP: out of memory writing the buffered rows of %s.  The FastBit "
                    "cache (warp_cache_size = %llu bytes) may be too small.",
                    share->data_dir_name, (unsigned long long)my_cache_size);
    rc = HA_ERR_OUT_OF_MEM;
  }
  try {
    writer->clearData();
  } catch(...) {
  }
  /*if(update_indexes) { 
    maintain_indexes(part_dir.c_str());
  }*/
  //delete writer;
  //writer = NULL;
  
  mysql_mutex_unlock(&share->mutex);
  return rc;
}

/*
  This is an INSERT.  The row data is converted to CSV (just like the CSV
  engine) and this is passed to the storage layer for processing.  It would be
  more efficient to construct a vector of rows to insert (to also support bulk
  insert).
*/
int ha_warp::write_row(uchar *buf) {
  DBUG_ENTER("ha_warp::write_row");
  try {
    DBUG_RETURN(write_row_impl(buf));
  } catch(...) {
    sql_print_error("WARP: out of memory buffering a row for %s.  The FastBit cache "
                    "(warp_cache_size = %llu bytes) may be too small.",
                    share->data_dir_name, (unsigned long long)my_cache_size);
  }
  /* The row may have been appended to some of the columns only.  FastBit
     would pad the others with default values when the buffer is written,
     so the buffered rows are discarded. */
  if(writer != NULL) {
    try {
      writer->clearData();
    } catch(...) {
    }
  }
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

int ha_warp::write_row_impl(uchar *buf) {
  DBUG_ENTER("ha_warp::write_row_impl");
  ha_statistic_increment(&System_status_var::ha_write_count);
  
  /* The row ids of this handler are consecutive.  If they came one at a time
     from a counter shared by all the connections that write to the table,
     the rows of a parallel load would have ids that are far apart. */
  if(local_rowids_left == 0) {
    local_next_rowid = warp_state->get_next_rowid_batch(WARP_HANDLER_ROWID_BATCH_SIZE);
    local_rowids_left = WARP_HANDLER_ROWID_BATCH_SIZE;
  }
  current_rowid = local_next_rowid--;
  --local_rowids_left;
    
  /* This will return a cached writer unless a background
    write was started on the last insert.  In that case
    a new writer is constructed because the old one will
    still be background writing.
  */
  if(create_writer(table)) {
    DBUG_RETURN(-1);
  }
  mysql_mutex_lock(&share->mutex);
  mysql_mutex_unlock(&share->mutex);
  /* The auto_increment value isn't being properly persisted between
    restarts at the moment.  AUTO_INCREMENT should definitely be
    considered and ALPHA level feature.
  */
  if(table->next_number_field && buf == table->record[0]) {
    int error;
    if((error = update_auto_increment())) DBUG_RETURN(error);
  }

  /* This encodes the data from the row buffer into a CSV string which
    is processed by Fastbit...  It is probably faster to construct a
    Fastbit row object but this is fast enough for now/ALPHA release.
  */
  ha_warp::encode_quote(buf);

  /* The writer object caches rows in memory.  Memory is reserved
    for a given number of rows, which defaults to 1 million.  The
    Fastbit cache size must be greater than or equal to this value
    or an allocation failure will happen.
  */
  writer->appendRow(buffer.c_ptr(), ",");
  stats.records++;
  
  /* In order to check for duplicate keys in a single insert
     statement the writer has to be flushed for each insert
     statement, which is not optimal - maybe there is a 
     better solution
  */
  /*if(unique_check_where_clause != "") {
    foreground_write();
    current_trx->write_insert_log_rowid(current_rowid);
  } else
  */
  if(writer->mRows() >= my_write_cache_size) {
    // write the rows to disk and destroy the writer (a new one will be created)
    DBUG_RETURN(write_buffered_rows_to_disk());
  }
  DBUG_RETURN(0);
}

// Updating a row in WARP is a bit weird.  A new version of the row is 
// written into the table and a LOCK_EX is taken on the row.  The
// delete bitmap isn't written until the transaction commits. The
// deleted row is written into the transaction log and it gets set
// when the log is read at commit which is quite different from
// InnoDB.  A history lock is also taken on the row.  During future
// scans this verion of the row will not be visible to this or 
// newer transactions and will be visible to older transactions.
int ha_warp::update_row(const uchar *, uchar *new_data) {
  DBUG_ENTER("ha_warp::update_row");
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  
  /*delete cursor;
  delete filtered_table;
  delete base_table;
  cursor = NULL;
  filtered_table = NULL;
  base_table = NULL;*/


  int lock_taken = warp_state->create_lock(current_rowid, current_trx, LOCK_EX);
  /* if deadlock or lock timeout return the error*/
  if(lock_taken != LOCK_EX) {
    DBUG_RETURN(lock_taken);
  }

  // current_rowid will be changed by write_row so save the 
  // value now
  uint64_t deleted_rowid = current_rowid;
  
  // if the write fails (for example due to duplicate key then
  // the statement will be rolled back and the deleted row 
  // will be 
  int retval = write_row(new_data);
    
  if(retval == 0) {
    // only log the delete and create the history lock 
    // if the write completed successfully.  The EX_LOCK
    // will still be held so the update can be retried
    // without having to lock the row again. 
    current_trx->write_delete_log_rowid(deleted_rowid);
    
    warp_state->create_lock(deleted_rowid, current_trx, LOCK_HISTORY);
  }
  
  ha_statistic_increment(&System_status_var::ha_update_count);

  
  DBUG_RETURN(retval);
}

/*
  Deletes a row. First the database will find the row, and then call this
  method. In the case of a table scan, the previous call to this will be
  the ::rnd_next() that found this row.
  The exception to this is an ORDER BY. This will cause the table handler
  to walk the table noting the positions of all rows that match a query.
  The table will then be deleted/positioned based on the ORDER (so RANDOM,
  DESC, ASC).
*/
int ha_warp::delete_row(const uchar *) {
  DBUG_ENTER("ha_warp::delete_row");
  
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  int lock_taken = warp_state->create_lock(current_rowid, current_trx, LOCK_EX);
  /* if deadlock or lock timeout return the error*/
  if(lock_taken != LOCK_EX) {
    DBUG_RETURN(lock_taken);
  }
  warp_state->create_lock(current_rowid, current_trx, LOCK_HISTORY);
  current_trx->write_delete_log_rowid(current_rowid);
  
  ha_statistic_increment(&System_status_var::ha_delete_count);
  stats.records--;
  DBUG_RETURN(0);
}

int ha_warp::delete_table(const char *table_name, const dd::Table *) {
  DBUG_ENTER("ha_warp::delete_table");

  // FIXME: this needs to be safer
  /* the files of the table are removed from the FastBit cache too, a table
     that is created with the same name must not read them */
  const std::string data_dir = std::string(table_name) + ".data";
  ibis::fileManager::instance().flushDir(data_dir.c_str());
  std::string cmdline =
      std::string("rm -rf ") + std::string(table_name) + ".data/";
  int rc = system(cmdline.c_str());
  ibis::fileManager::instance().flushDir(data_dir.c_str());
  ha_statistic_increment(&System_status_var::ha_delete_count);
  DBUG_RETURN(rc != 0);

}

// int ha_warp::truncate(dd::Table *dd) {
int ha_warp::delete_all_rows() {
  int rc = delete_table(share->table_name.c_str(), NULL);
  if(rc) return rc;
  return create(share->table_name.c_str(), table, NULL, NULL);
}

WARP_SHARE* ha_warp::get_warp_share() {
  return share;
}
/*
  ::info() is used to return information to the optimizer.
  Currently this table handler doesn't implement most of the fields
  really needed. SHOW also makes use of this data
*/
int ha_warp::info(uint) {
  DBUG_ENTER("ha_warp::info");
  std::unordered_map<std::string, bool> query_tables;
  close_in_extra = true;
  
  auto table_counts = get_table_counts_in_schema(share->data_dir_name);

  auto thd = current_thd;
  auto cur_table = thd->open_tables;
  
  while(cur_table != NULL) {
    auto handler = (ha_warp*)(cur_table->file);
    auto other_share = handler->get_warp_share();
    query_tables.emplace(std::make_pair(other_share->data_dir_name, true));
    cur_table=cur_table->next;
  }
  
  const char* table_with_most_rows = get_table_with_most_rows(&table_counts, query_tables);
  uint64_t least_row_count = get_least_row_count(&table_counts);
  assert(table_with_most_rows != NULL);
  bool is_fact_table = false;

  // list the tables in the query
  // if this is the fact table (largest table in schema) set the records to the smallest possible value
  // which is 2 (otherwise const evaluation will be used)
  if(strstr(share->data_dir_name, table_with_most_rows) != NULL) {
    is_fact_table = true;
    if(THDVAR(table->in_use, adjust_table_stats_for_joins)) {
      stats.records = least_row_count+2;
    }
  } 

  stats.mean_rec_length = 0;
  for (Field **field = table->s->field; *field; field++) {
    switch((*field)->real_type()) {
      case MYSQL_TYPE_TINY:
        stats.mean_rec_length += 1;
        break;

      case MYSQL_TYPE_SHORT:
        stats.mean_rec_length += 2;
        break;

      case MYSQL_TYPE_INT24:
        stats.mean_rec_length += 3;
        break;

      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
        stats.mean_rec_length += 4;
        break;

      case MYSQL_TYPE_LONGLONG:
        stats.mean_rec_length += 8;
        break;

      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_JSON:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_GEOMETRY:
      default:
        /* this is a total lie but this is just an estimate */
        stats.mean_rec_length += 8;
        break;
    }

    stats.auto_increment_value = stats.records;
  }

  /* estimate the data size from the record count and average record size */
  stats.data_file_length = stats.mean_rec_length * stats.records;
  
  /* register the table for condition pushdown..  ::info is always called before 
     ::engine_push so this ensures the table information for hybrid join is
     available when we get there
  */
  warp_pushdown_information* pushdown_info = get_or_create_pushdown_info(table->in_use, table->alias, share->data_dir_name);
  pushdown_info->fields = table->s->field;
  set_column_set();

  pushdown_info->column_set = column_set;

  if(is_fact_table) {
    pushdown_info->is_fact_table = true;
  }

  DBUG_RETURN(0);
}

int file_exists(const char *file_name) {
  struct stat buf;
  return (stat(file_name, &buf) == 0);
}

int file_exists(std::string file_name) {
  return file_exists(file_name.c_str());
}

void index_builder(ibis::table* tbl, const char* cname, const char* comment) {
  tbl->buildIndex(cname, comment);
}
/* Fastbit will normally maintain the indexes automatically, but if the type
   of bitmap index is to be set manually, the comment on the field will be
   taken into account. */
/* Called when a scan is done with a partition (the FastBit tables that read
   it are deleted).  The column files of the partition stay in the FastBit
   cache so that a later query finds them there, but FastBit does not evict
   idle files to make room for the arrays queries allocate.  A scan of a
   large table would fill the cache with files of partitions that it has
   already passed and fail with "out of memory" although most of the cache
   is idle.  Once a quarter of the cache is gone, the files of the
   partitions that were scanned are dropped. */
/* The cache space that is kept free for what a scan allocates (the columns
   selected from the partitions that are read, the partitions read ahead and
   the join workers).  A quarter of the cache, or about 256 bytes for every
   row of a partition if that is more: a small cache is then always flushed. */
static uint64_t warp_cache_reserve() {
  return std::max<uint64_t>(ibis::fileManager::currentCacheSize() / 4,
                            (uint64_t)my_partition_max_rows * 256);
}

static void warp_release_partition(const char *partition_dir) {
  if (partition_dir == NULL) {
    return;
  }
  if (ibis::fileManager::bytesFree() < warp_cache_reserve()) {
    ibis::fileManager::instance().flushDir(partition_dir);
  }
}

/* Brings the indexes of the columns of a partition up to date.  The
   function does not use the handler, so look ahead threads can run it. */
static void warp_maintain_partition_indexes(const char *datadir,
                                            const std::vector<uint> &field_indexes) {
  /* the scans that run at the same time would load, rebuild and unload the
     same index files */
  static std::mutex maintain_mtx;
  std::lock_guard<std::mutex> maintain_guard(maintain_mtx);
  std::unique_ptr<ibis::part> tbl(new ibis::part(datadir));
  for (uint field_index : field_indexes) {
    std::string columnIndexFilename = std::string(datadir) + "/c" + std::to_string(field_index) + ".idx";
    if(file_exists(columnIndexFilename)) {
      ibis::fileManager::instance().flushFile(columnIndexFilename.c_str());
      auto col = tbl->getColumn(field_index);
      if(col->hasIndex()) {
        col->loadIndex();
        if (col->indexedRows() != tbl->nRows() ) {
          // update the index if the existing one does not
          // have the same number of rows as the current data
          // partition.  The file is removed and made again, not written
          // over: scans of other connections may have the old file mapped
          // into memory, and a file that shrinks under a mapping kills the
          // process.  Those scans keep the old file, and the new one is made
          // while the lock of the index file is held.
          col->unloadIndex();
          col->purgeIndexFile();
          col->loadIndex();
        }
        col->unloadIndex();
      }
    }
  }
}

void ha_warp::maintain_indexes(const char *datadir) {
  std::vector<uint> field_indexes;
  for (Field **field = table->field; *field; field++) {
    field_indexes.push_back((*field)->field_index());
  }
  warp_maintain_partition_indexes(datadir, field_indexes);
}

/* Starts opening and selecting the partition in a thread of the thread
   budget of the statement.  Returns false if there is no thread to spare
   (the partition is then opened by the connection thread when it is
   needed). */
bool ha_warp::start_prefetch(const std::string& datadir) {
  /* The partitions read ahead take cache space, so nothing is read ahead
     when the cache is nearly full.  A quarter of the cache is kept free by
     warp_release_partition, files of earlier queries stay in the rest. */
  if(ibis::fileManager::bytesFree() < warp_cache_reserve()) {
    return false;
  }
  auto budget = ibis::util::ThreadBudget::current();
  if(!budget || budget->tryAcquire(1) == 0) {
    return false;
  }
  std::vector<uint> field_indexes;
  for (Field **field = table->field; *field; field++) {
    field_indexes.push_back((*field)->field_index());
  }
  const std::string columns = column_set;
  const std::string where = push_where_clause;
  const std::string table_dir = share->data_dir_name;
  try {
    prefetched_partitions.emplace(datadir, std::async(std::launch::async,
      [budget, datadir, field_indexes, columns, where, table_dir]() {
        ibis::util::ThreadBudgetScope budget_scope(budget);
        prefetched_partition result;
        try {
          warp_table_read_lock data_lock(table_dir.c_str());
          result.base_table = ibis::table::create(datadir.c_str());
          if(result.base_table != NULL) {
            warp_maintain_partition_indexes(datadir.c_str(), field_indexes);
            result.filtered_table = result.base_table->select(columns.c_str(), where.c_str());
            /* the rows were copied, see ha_warp::release_scanned_table */
            delete result.base_table;
            result.base_table = NULL;
          }
        } catch(...) {
          delete result.filtered_table;
          result.filtered_table = NULL;
        }
        budget->release(1);
        return result;
      }));
  } catch(...) {
    budget->release(1);
    return false;
  }
  return true;
}

/* Keeps the next few partitions that will be returned being prepared. */
void ha_warp::prefetch_following_partitions() {
  static const size_t depth = 3;
  if(partitions == NULL || ibis::util::ThreadBudget::current() == nullptr) {
    return;
  }
  auto it = part_it;
  if(it == partitions->end()) {
    return;
  }
  for(++it; it != partitions->end() && prefetched_partitions.size() < depth; ++it) {
    if(*it == NULL) {
      continue;
    }
    const std::string dir((*it)->currentDataDir());
    if(dir == std::string(share->data_dir_name)) {
      continue; // the top level directory has no rows
    }
    if(matching_ridset.size() > 0) {
      auto find_it = matching_ridset.find(dir);
      if(find_it == matching_ridset.end() || find_it->second == NULL) {
        continue; // no row of this partition is returned
      }
    }
    if(prefetched_partitions.count(dir) > 0) {
      continue;
    }
    if(!start_prefetch(dir)) {
      break;
    }
  }
}

/* Takes the tables opened by a look ahead thread, waiting for it if it is
   still working.  Returns false if the partition was not prefetched. */
bool ha_warp::take_prefetched(const std::string& datadir, ibis::table** base,
                              ibis::table** filtered) {
  auto find_it = prefetched_partitions.find(datadir);
  if(find_it == prefetched_partitions.end()) {
    return false;
  }
  prefetched_partition result = find_it->second.get();
  prefetched_partitions.erase(find_it);
  *base = result.base_table;
  *filtered = result.filtered_table;
  return true;
}

/* The table that select was run on is not needed to return the rows, they
   were copied into filtered_table.  If it were kept until the scan ends, the
   column and index files of the partition that it uses would stay "in use" in
   the FastBit cache all that time.  A cached file in use can not be removed,
   so when another connection appends rows to the partition and its files are
   flushed from the cache, the old copy of an index (or of a column) stays,
   and the first statements that follow do not see the new rows. */
void ha_warp::release_scanned_table() {
  if(filtered_table != NULL) {
    delete base_table;
    base_table = NULL;
  }
}

void ha_warp::discard_prefetched() {
  for(auto &entry : prefetched_partitions) {
    prefetched_partition result = entry.second.get();
    delete result.filtered_table;
    delete result.base_table;
  }
  prefetched_partitions.clear();
}

/* The ::extra function is called a bunch of times before and after various
   storage engine operations. I think it is used as a hint for faster alter
   for example. Right now, in warp if there are any dirty rows buffered in
   the writer object, flush them to disk when ::extra is called.   Seems to
   work.
*/
int ha_warp::extra(enum ha_extra_function xtra) {
  if(xtra == 0) {
    return -1;
  }
  /* if not bulk insert, and there are buffered inserts, write them out
     to disk.  This will destroy the writer.
  */
  /*if(writer != NULL) {
    write_buffered_rows_to_disk();
  }*/
  return 0;
}

void ha_warp::cleanup_pushdown_info() {
  star_agg.reset();
  if(index_scan_open) {
    end_scan(true);
  }
  index_base_where = "";
  // free up memory used for pushdown filters
  auto pushdown_info = get_pushdown_info(table->in_use, table->alias);
  
  pushdown_mtx.lock();
  auto it = pd_info.find(table->in_use);
  if(it != pd_info.end()) {
  
    // remove the pushdown info for this table
    auto it2 = it->second->find(table->alias);
    if(it2 != it->second->end()) {
      it->second->erase(it2);
    }
    // if all of the tables are removed delete the pushdown info completely
    if(it->second->size() == 0) {
      delete it->second;
      pd_info.erase(it);
    }

  }
  
  if(pushdown_info != NULL) {
    /* the pushdown information owns the FastBit objects stored on it,
       whether they were opened by the join pushdown or by a scan */
    delete pushdown_info->cursor;
    delete pushdown_info->filtered_table;
    delete pushdown_info->base_table;
  }
  delete pushdown_info;
  pushdown_mtx.unlock();

  /* end of the statement: nothing pushed down for it may be used again */
  push_where_clause = "";
  current_matching_dim_ridset = NULL;
  current_matching_ridset = NULL;

  for(auto &filter : fact_table_filters) {
    retired_fact_table_filters.push_back(filter);
  }
  fact_table_filters.clear();
  for(auto &filter : retired_fact_table_filters) {
    delete filter.first;
    delete filter.second;
  }
  retired_fact_table_filters.clear();
  pushdown_table_count = 0;
  bitmap_merge_join_executed = false;
}  

int ha_warp::repair(THD *, HA_CHECK_OPT *) {
  DBUG_ENTER("ha_warp::repair");
  my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "REPAIR is not supported");
  DBUG_RETURN(HA_ERR_UNSUPPORTED);
}

/*
  Called by the database to lock the table. Keep in mind that this
  is an internal lock.
*/
THR_LOCK_DATA **ha_warp::store_lock(THD *thd, THR_LOCK_DATA **to,
                                    enum thr_lock_type lock_type) {
  DBUG_ENTER("ha_warp::store_lock");

  lock_in_share_mode = false;
  lock_for_update = false;

  if(lock_type == TL_READ_WITH_SHARED_LOCKS) {
    lock_in_share_mode = true;
  }

  if(lock_type == TL_WRITE && thd->lex->sql_command == SQLCOM_SELECT) {
    lock_for_update = true;
  }

  if(lock_type != TL_IGNORE && lock.type == TL_UNLOCK) {
    lock.type = lock_type;
  }

  DBUG_RETURN(to);
}

int ha_warp::create_writer(TABLE *table_arg) {
  if(writer != NULL) {
    return 0;
  }
  
  /*
  Add the columns of the table to the writer object.

  MySQL types map to IBIS types:
  -----------------------------------------------------
  UNKNOWN_TYPE, OID, UDT, CATEGORY, BIT, BLOB
  BYTE, UBYTE , SHORT , USHORT, INT, UINT, LONG, ULONG
  FLOAT, DOUBLE
  TEXT
  */
  int column_count = 0;
  const char *index_spec=NULL;
  /* Create an empty writer.  Columns are added to it */
  writer = ibis::tablex::create();

  for (Field **field = table_arg->s->field; *field; field++) {
    std::string name = "c" + std::to_string(column_count);
    std::string nname = "n" + std::to_string(column_count);
    ++column_count;

    ibis::TYPE_T datatype = ibis::UNKNOWN_TYPE;
    bool is_unsigned = (*field)->all_flags() & UNSIGNED_FLAG;
    bool is_nullable = (*field)->is_nullable();

    /* create a tablex object to create the metadata for the table */
    switch((*field)->real_type()) {
      case MYSQL_TYPE_TINY:
        index_spec="<binning none/><encoding interval/>";
        if(is_unsigned) {
          datatype = ibis::UBYTE;
        } else {
          datatype = ibis::BYTE;
        }
        break;

      case MYSQL_TYPE_SHORT:
        index_spec="<binning none/><encoding interval-equality/>";
        if(is_unsigned) {
          datatype = ibis::USHORT;
        } else {
          datatype = ibis::SHORT;
        }
        break;

      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
        index_spec="<binning none><encoding binary/>";
        if(is_unsigned) {
          datatype = ibis::UINT;
        } else {
          datatype = ibis::INT;
        }
        break;

      case MYSQL_TYPE_LONGLONG:
        index_spec="<binning none/><encoding binary/>";
        if(is_unsigned) {
          datatype = ibis::ULONG;
        } else {
          datatype = ibis::LONG;
        }
        break;

      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_JSON:
        /*CHARSET NUMBER:46
          CHARSET M_COL_NAME:utf8mb4_bin*/
        if((*field)->charset()->number != 46) {
          std::string errmsg = "Unsupported character set or collation for column (only utf8mb4/utf8mb4_bin supported): " +
                             std::string((*field)->field_name);
          my_error(1031, MYF(0), errmsg.c_str());
          return 1;
        }
        if(strcmp((*field)->charset()->m_coll_name, "utf8mb4_bin")) {
          std::string errmsg = "Unsupported character set or collation for column (only utf8mb4/utf8mb4_bin supported): " +
                             std::string((*field)->field_name);
          my_error(1031, MYF(0), errmsg.c_str());
          return 1;
        }
        index_spec="<binning none/><encoding binary/>";
        datatype = ibis::TEXT;
        break;

      case MYSQL_TYPE_FLOAT:
        index_spec="<binning precision=2/><encoding interval-equality/>";
        datatype = ibis::FLOAT;
        break;

      case MYSQL_TYPE_DOUBLE:
        index_spec="<binning precision=2/><encoding interval-equality/>";
        datatype = ibis::DOUBLE;
        break;

      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
        index_spec="<binning none/><encoding binary/>";
        datatype = ibis::TEXT;
        break;

      case MYSQL_TYPE_YEAR:
        index_spec="<binning none/><encoding interval-equality/>";
        datatype = ibis::USHORT;
        break;

      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
     /*   index_spec="<binning none/><encoding interval-equality/>";
        datatype = ibis::UINT;
        break;*/

      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_DATETIME2:
        index_spec="<binning none/><encoding interval-equality/>";
        datatype = ibis::ULONG;
        break;

      case MYSQL_TYPE_ENUM:
        datatype = ibis::CATEGORY;
        break;

      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_GEOMETRY:
        datatype = ibis::TEXT;
        break;

      /* UNSUPPORTED TYPES */
      default:
        std::string errmsg = "Unsupported data type for column: " +
                             std::string((*field)->field_name);
        my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), errmsg.c_str());
        datatype = ibis::UNKNOWN_TYPE;
        return 1;
        break;
    }
    /* Fastbit supports numerous bitmap index options.  You can place these
       options in the comment string of a column.  When an index is created on a
       column, then the indexing options used are taken from the comment.  In
       the future, the comment will support more options for compression, etc.
    */
    const char *custom_spec;
    if((custom_spec = strstr((*field)->comment.str, "index=")) != NULL) {
      index_spec = custom_spec+6;  
      if(index_spec[0] != '<') {
        std::string tmp_spec = "<" + std::string(custom_spec) + "/>";
        index_spec = tmp_spec.c_str();
      }
    } else {
      if(index_spec == NULL) { 
        index_spec = "<binary/>";
      }
    }
    
    writer->addColumn(name.c_str(), datatype, NULL, index_spec);

    /* Columns which are NULLable have a NULL marker.  A better approach might
       to have one NULL bitmap stored as a separate column instead of one byte
       per NULLable column, but that makes query processing a bit more complex
       so this simpler approach is taken for now.  Also, once compression is
       implemented, these columns will shrink quite a bit.
    */
    if(is_nullable) {
      // correspondingly numbered column");
      writer->addColumn(nname.c_str(), ibis::UBYTE,
                        "NULL marker for the correspondingly numbered column",
                        "<binning none/><encoding equality/>");
                        //"none");
    }
  }
  /* This is the psuedo-rowid which is used for deletes and updates */
  writer->addColumn("r", ibis::ULONG, "WARP rowid");

    /* This is the psuedo-rowid which is used for deletes and updates */
  writer->addColumn("t", ibis::ULONG, "WARP transaction identifier");

  /* This is the memory buffer for writes*/
  //writer->reserveBuffer(my_write_cache_size > my_partition_max_rows
  //                          ? my_partition_max_rows
  //                          : my_write_cache_size);

  /* FIXME: should be a table option and should be able to be set in size not
   * just count*/
  writer->setPartitionMax(my_partition_max_rows);
  mysql_mutex_lock(&share->mutex);
  mysql_mutex_unlock(&share->mutex);

  return 0;
}

/*
  Create a table. You do not want to leave the table open after a call to
  this (the database will call ::open() if it needs to).

  Note that the internal Fastbit columns are named after the field numbers
  in the MySQL table.
*/
// int ha_warp::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *info,
static bool warp_key_part_supported(const KEY_PART_INFO *kp);

int ha_warp::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *,
                    dd::Table *) {
  DBUG_ENTER("ha_warp::create");
  int rc = 0;
  /* the indexes are declarations, see ha_warp::make_key_condition */
  for(uint k = 0; k < table_arg->s->keys; ++k) {
    const KEY *key = &table_arg->key_info[k];
    if(key->flags & (HA_NOSAME | HA_FULLTEXT | HA_SPATIAL)) {
      my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), "WARP",
               (key->flags & HA_NOSAME)  ? "UNIQUE or PRIMARY KEY"
               : (key->flags & HA_FULLTEXT) ? "FULLTEXT index"
                                            : "SPATIAL index");
      DBUG_RETURN(-1);
    }
    for(uint p = 0; p < key->user_defined_key_parts; ++p) {
      if(!warp_key_part_supported(&key->key_part[p])) {
        my_error(ER_WRONG_KEY_COLUMN, MYF(0), key->key_part[p].field->field_name);
        DBUG_RETURN(-1);
      }
    }
  }
  if(!(share = get_share(name, table))) DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  /* create the writer object from the list of columns in the table 
     if non-zero is returned return error 1030 - unsupported option*/
  if(create_writer(table_arg)) {
    DBUG_RETURN(1031);
  }

  char errbuf[MYSYS_STRERROR_SIZE];
  DBUG_PRINT("ha_warp::create",
             ("creating table data directory=%s", share->data_dir_name));
  if(file_exists(share->data_dir_name)) {
    delete_table(name, NULL);
  }
  if(mkdir(share->data_dir_name, S_IRWXU | S_IXOTH) == -1) {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             my_strerror(errbuf, sizeof(errbuf), errno));
    DBUG_RETURN(-1);
  }

  /* Write the metadata to disk (returns 1 on success but this function returns
     0 on success...) Be nice and try to clean up if metadata write failed (out
     of disk space for example)
  */
  DBUG_PRINT("ha_warp::create", ("Writing table metadata"));
  if(!writer->writeMetaData((std::string(share->data_dir_name)).c_str())) {
    if(file_exists(share->data_dir_name)) {
      rmdir(share->data_dir_name);
    }
    rc = -1;
  }

  DBUG_RETURN(rc);
}

int ha_warp::check(THD *, HA_CHECK_OPT *) {
  DBUG_ENTER("ha_warp::check");
  DBUG_RETURN(HA_ADMIN_OK);
}

/* OPTIMIZE TABLE rewrites every column data file (and the starting
   position files of string columns) in the format selected
   by warp_compression: compressed files are re-framed into full size zstd
   frames (merging the small frames created by many small inserts) and
   plain files are compressed, or, if compression is disabled, compressed
   files are converted back to plain files.  The logical content of the
   files does not change. */
int ha_warp::optimize(THD *, HA_CHECK_OPT *) {
  DBUG_ENTER("ha_warp::optimize");
  int rc = HA_ADMIN_OK;
  const bool compress = ibis::zfile::level() > 0;
  mysql_mutex_lock(&share->mutex);
  ibis::partList parts;
  ibis::util::gatherParts(parts, share->data_dir_name, true);
  for (auto part : parts) {
    const char *dir = part->currentDataDir();
    if (dir == nullptr || part->nRows() == 0) continue;
    for (uint32_t i = 0; i < part->nColumns(); ++i) {
      const ibis::column *col = part->getColumn(i);
      if (col == nullptr || col->type() == ibis::BLOB) continue;
      std::vector<std::string> files;
      files.push_back(std::string(dir) + FASTBIT_DIRSEP + col->name());
      /* string columns also have a file with the starting positions */
      if (col->type() == ibis::TEXT || col->type() == ibis::CATEGORY)
        files.push_back(files[0] + ".sp");
      for (const auto &fname : files) {
        if (ibis::util::getFileSize(fname.c_str()) <= 0) continue;
        int ierr = 0;
        if (compress) {
          ierr = ibis::zfile::compact(fname.c_str());
        } else if (ibis::zfile::isCompressed(fname.c_str())) {
          std::string content;
          ierr = ibis::zfile::readAll(fname.c_str(), content);
          if (ierr == 0)
            ierr = ibis::zfile::writeWhole(fname.c_str(), content.data(),
                                           content.size(), 0, false);
        }
        if (ierr < 0) {
          sql_print_error("WARP: OPTIMIZE failed to rewrite %s (error %d)",
                          fname.c_str(), ierr);
          rc = HA_ADMIN_FAILED;
        }
      }
    }
    ibis::fileManager::instance().flushDir(dir);
  }
  for (auto part : parts) delete part;
  mysql_mutex_unlock(&share->mutex);
  DBUG_RETURN(rc);
}

bool ha_warp::check_if_incompatible_data(HA_CREATE_INFO *, uint) {
  return COMPATIBLE_DATA_YES;
}

/* An index of a WARP table is a declaration, there is no index structure to
   build or to drop.  An ALTER TABLE that only adds, drops or renames
   non-unique indexes, or changes their comment or visibility, is a change of
   the definition of the table in the data dictionary and does not touch the
   data.  Everything else is done by copying the table, which is also where
   keys that WARP does not support are refused (see ha_warp::create). */
enum_alter_inplace_result ha_warp::check_if_supported_inplace_alter(
    TABLE *altered_table, Alter_inplace_info *ha_alter_info) {
  DBUG_ENTER("ha_warp::check_if_supported_inplace_alter");
  static const Alter_inplace_info::HA_ALTER_FLAGS index_changes =
      Alter_inplace_info::ADD_INDEX | Alter_inplace_info::DROP_INDEX |
      Alter_inplace_info::RENAME_INDEX | Alter_inplace_info::ALTER_INDEX_COMMENT;
  const Alter_inplace_info::HA_ALTER_FLAGS flags = ha_alter_info->handler_flags;

  /* only index changes, and at least one */
  if((flags & ~index_changes) != 0 || (flags & index_changes) == 0) {
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
  }
  /* every index of the new definition must be one that WARP accepts (the
     ones that stay were checked when the table was created) */
  for(uint k = 0; k < altered_table->s->keys; ++k) {
    const KEY *key = &altered_table->key_info[k];
    if(key->flags & (HA_NOSAME | HA_FULLTEXT | HA_SPATIAL)) {
      DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
    }
    for(uint p = 0; p < key->user_defined_key_parts; ++p) {
      if(key->key_part[p].field == nullptr || !warp_key_part_supported(&key->key_part[p])) {
        DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
      }
    }
  }
  /* The server must be told "in place" if the statement asks for ALGORITHM=INPLACE;
     "instant" otherwise.  Nothing is locked: no data is read or written. */
  if(ha_alter_info->alter_info->requested_algorithm ==
     Alter_info::ALTER_TABLE_ALGORITHM_INPLACE) {
    DBUG_RETURN(HA_ALTER_INPLACE_NO_LOCK);
  }
  DBUG_RETURN(HA_ALTER_INPLACE_INSTANT);
}

/* This is where table scans happen.  While most storage engines
   scan ALL rows in this function, the WARP engine supports
   engine condition pushdown.  This means that the WHERE clause in
   the SQL statement is made available to the WARP engine for
   processing during the scan.

   This has MAJOR performance implications.

   Fastbit can evaluate and satisfy with indexes many complex
   conditions that MySQL itself can not support efficiently
   (or at all) with btree or hash indexes.

   These include conditions such as:
   col1 = 1 OR col2 = 1
   col1 < 10 and col2 between 1 and 2
   (col1 = 1 or col2 = 1) and col3 = 1

   Fastbit evaluation will bitmap intersect the index results
   for each evaluated expression.  Fastbit will automatically
   construct indexes for these evaluations when appropriate.
*/
int ha_warp::rnd_init(bool) {
  DBUG_ENTER("ha_warp::rnd_init");
  if(star_agg) {
    /* the query is aggregated by the engine, the rows are the groups */
    DBUG_RETURN(star_agg_run() ? HA_ERR_INTERNAL_ERROR : 0);
  }
  if(!index_scan_mode && index_scan_open) {
    /* the scan of an index lookup that the statement left open */
    end_scan(false);
  }
  discard_prefetched();
  fetch_count = 0;
  auto pushdown_info = get_pushdown_info(table->in_use, table->alias);
  if(pushdown_info == NULL) {
    /* ::info creates the pushdown information while a statement is
       optimized.  A replica applying row events looks up rows without it. */
    pushdown_info = get_or_create_pushdown_info(table->in_use, table->alias, share->data_dir_name);
    pushdown_info->fields = table->s->field;
  }
  char* partition_filter = THDVAR(table->in_use, partition_filter);
  full_partition_scan = false;
  /* extract/use the partition filter if provided*/
  uint partition_filter_len = strlen(partition_filter);
  partition_filter_alias = "";
  partition_filter_partition_name = "";
  // partition filter is of form alias: pX 
  // minimum alias is one char, plus two char delim, plus two chars for partition = 5 chars
  
  if(partition_filter_len >= 5) {
    for(uint delim_at = 0;delim_at<partition_filter_len;delim_at++) {
      if(partition_filter[delim_at] == ':' && partition_filter[delim_at+1] == ' ') {
        partition_filter_alias.assign(partition_filter, delim_at);
      
        if(partition_filter_alias == table->alias) {
          partition_filter_partition_name.assign(partition_filter + delim_at+2);
        }
        break;
      }
    }
  }
  
  current_rowid = 0;
  /* a scan that was ended early (an error, a LIMIT, ...) leaves these
     pointing to a join filter of the previous statement, which is freed
     at the end of the statement */
  current_matching_dim_ridset = NULL;
  current_matching_ridset = NULL;
  /* When scanning this is used to skip evaluation of transactions
     that have already been evaluated
  */  
  last_trx_id = 0;

  /* This is the a big part of the performance advantage of WARP outside of
     the bitmap indexes.  This figures out which columns this query is reading
     so we only read the necesssary columns from disk.

     This is the primary advantage of a column store.
  */
  set_column_set();

  /* push_where_clause is populated in ha_warp::cond_push() which is the
     handler function invoked by engine condition pushdown.  When ECP is
     used, then push_where_clause will be a non-empty string.  If it
     isn't used, then the WHERE clause is set such that Fastbit will
     return all rows.
  */
  if(push_where_clause == "") {
    push_where_clause = "1=1";
  }
  
  /* The dimension tables of a star join are read from the pushdown
     information only if the join of the fact table has made its filters;
     without them the table is scanned as usual (MySQL does the join). */
  if(!index_scan_mode && pushdown_info->base_table != NULL &&
     pushdown_info->fact_table_filters != NULL) {
    partitions = NULL;
    /* these objects belong to the pushdown information */
    scan_uses_pushdown_tables = true;
    base_table = pushdown_info->base_table;
    filtered_table = pushdown_info->filtered_table;
    if(filtered_table != NULL && pushdown_info->cursor != NULL) {
      cursor = pushdown_info->cursor;
    } else {
      if(filtered_table != NULL) {
        cursor = filtered_table->createCursor();
        pushdown_info->cursor=cursor; 
      }
    }
 
    for(auto filter_it = pushdown_info->fact_table_filters->begin(); filter_it != pushdown_info->fact_table_filters->end(); ++filter_it) {
 
      if((filter_it->first)->dim_alias == std::string(table->alias)) {
        auto tmp = (filter_it->first);
        current_matching_dim_ridset_it = tmp->get_rownums()->begin();
 
        current_matching_dim_ridset = tmp->get_rownums();
        
        break;
      }
    }
    rownum = 0;
    
  } else {
    base_table = NULL;
    /* The partitions are found by reading their metadata, which a writer that
       appends to a partition rewrites.  A partition whose metadata is read
       at that moment is not found, the scan then returns no rows at all. */
    warp_table_read_lock partitions_lock(share->data_dir_name);
    if( (get_pushdown_info_count(current_thd) > 1 && pushdown_info->is_fact_table) || partition_filter_partition_name != "" ) {
      partitions = new ibis::partList;
    
      // read all partitions unless a filter is set
      if(partition_filter_partition_name == "") {
        ibis::util::gatherParts(*partitions, share->data_dir_name, true);
        part_it = partitions->begin();
      } else {
        // only read one partition if filter is set
        std::string tmpstr = std::string(share->data_dir_name);
        tmpstr += "/" + partition_filter_partition_name;
        ibis::util::gatherParts(*partitions, tmpstr.c_str(), true);
        part_it = partitions->begin();
      }
    } else {
      partitions = new ibis::partList;
      full_partition_scan = true;
      ibis::util::gatherParts(*partitions, share->data_dir_name, true);
      part_it = partitions->begin();
    }
  }
  
  DBUG_RETURN(0);
}

void filter_fact_column(
  ibis::query* column_query, 
  fact_table_filter::iterator fact_filter, 
  std::vector<uint32_t>* matching_rids, 
  std::set<uint64_t>* matching_dim_rids,
  uint32_t* running_filter_threads,
  std::mutex* fact_filter_mutex ) 
{ 
  auto column_vals = column_query->getQualifiedLongs((fact_filter->first)->fact_column.c_str());
    
  uint32_t rownum = 1;
  std::vector<uint64_t> matching_dim_rowids;
  matching_dim_rowids.clear();
  for(auto column_it = column_vals->begin(); column_it != column_vals->end(); ++column_it) {
    ++rownum;
    auto find_it = fact_filter->second->find(*column_it);
    if( find_it != fact_filter->second->end() ) {
      matching_rids->push_back(rownum);
      matching_dim_rids->insert(find_it->second);
    }
  }
  delete column_vals;
  column_vals = NULL;
  
  fact_filter_mutex->lock();
  --(*running_filter_threads);
  fact_filter_mutex->unlock();
  
}
// this is expensive because it has to try to insert many existing entries
// into the existing set.  This presents a synchronization point in the 
// join process to it is moved into a dedicated thread
// there should be one running threads per dimension because a
// mutex is held as each dimension is processed

void merge_dimension_keys(
  fact_table_filter::iterator filter_it, 
  std::set<uint64_t>* matching_dim_rowids, 
  uint32_t* running_dimension_merges, 
  std::mutex* dimension_merge_mutex ) {
 
  try {
    filter_it->first->mtx.lock();
    for(auto insert_it = matching_dim_rowids->begin(); insert_it != matching_dim_rowids->end(); ++insert_it) {
      filter_it->first->add_matching_rownum(*insert_it);
    }
    filter_it->first->mtx.unlock();
  } catch(...) {
    /* out of memory while merging: the filter is incomplete */
    filter_it->first->mtx.unlock();
    filter_it->first->failed = true;
  }
  delete matching_dim_rowids;
  
  dimension_merge_mutex->lock();

  (*running_dimension_merges)--;
  dimension_merge_mutex->unlock();
}

void exec_pushdown_join(
  ibis::query* column_query, 
  ibis::partList::iterator part_it, 
  fact_table_filter* fact_table_filters,
  std::unordered_map<std::string, std::vector<uint32_t>*>* matching_ridset,
  uint32_t* running_join_threads, 
  std::mutex* parallel_join_mutex,
  uint32_t* running_dimension_merges,
  std::mutex* dimension_merge_mutex,
  std::atomic<int>* join_error,
  size_t min_slice_rows ) {

  /* Any failure in this thread leaves the matching rows of this partition
     incomplete.  It is recorded in join_error so that the scan fails
     instead of returning wrong results. */
  try {
  /* match_count[i] is the number of filters that row i+1 of the rows of
     the query has satisfied so far.  Every row is looked up in the
     dimension keys of the first filter, later filters only look up the
     rows that satisfied all previous filters. */
  std::vector<uint8_t> match_count;

  uint8_t filter_exec_count = 0;
  auto filter_it = fact_table_filters->begin();
  
  for ( filter_exec_count = 1; filter_it != fact_table_filters->end(); ++filter_it,++filter_exec_count) {
    auto column_vals = column_query->getQualifiedLongs((filter_it->first)->fact_column.c_str());
    if(!column_vals) {
      /* usually the FastBit cache is too small to read the column */
      throw std::runtime_error("could not read column " + (filter_it->first)->fact_column);
    }
    std::unique_ptr<ibis::array_t<int64_t>> column_vals_guard(column_vals);
    
    const size_t nvals = column_vals->size();
    if( filter_exec_count == 1 ) {
      match_count.assign(nvals, 0);
    } else if( match_count.size() != nvals ) {
      throw std::runtime_error("the join column " + (filter_it->first)->fact_column + " has a different number of values");
    }
    auto matching_dim_rowids = new std::set<uint64_t> ;
    std::unique_ptr<std::set<uint64_t>> matching_dim_rowids_guard(matching_dim_rowids);

    /* The look ups of the rows [lo, hi) are independent of each other:
       the dimension keys are only read and every row has its own counter.
       The rows are split into slices that are probed at the same time if
       the thread budget of the statement has threads to spare.  The
       dimension rows of a slice are collected in a vector of its own. */
    const std::unordered_map<uint64_t, uint64_t>* dim_keys = filter_it->second;
    const int64_t* vals = column_vals->begin();
    uint8_t* counts = match_count.data();
    const uint8_t previous_count = filter_exec_count - 1;
    auto probe = [=](size_t lo, size_t hi, std::vector<uint64_t>* dim_rows) {
      for(size_t i = lo; i < hi; ++i) {
        /* the lookups into the counters are 8 bit and make the second
           and later passes considerably faster than the key lookups */
        if( counts[i] != previous_count ) {
          continue;
        }
        auto find_it = dim_keys->find(vals[i]);
        if( find_it == dim_keys->end() ) {
          continue;
        }
        counts[i] = filter_exec_count;
        dim_rows->push_back(find_it->second);
      }
    };

    const size_t min_slice = std::max<size_t>(min_slice_rows, 1);
    size_t nslices = 1;
    if( nvals >= 2 * min_slice ) {
      nslices = std::min<size_t>(nvals / min_slice, 64);
    }
    ibis::util::ThreadLease lease(nslices > 1 ? nslices - 1 : 0);
    nslices = lease.granted() + 1;
    std::vector<std::vector<uint64_t>> slice_dim_rows(nslices);
    std::vector<std::exception_ptr> slice_errors(nslices);
    auto run_slice = [&](size_t k) {
      try {
        probe(nvals * k / nslices, nvals * (k + 1) / nslices, &slice_dim_rows[k]);
      } catch(...) {
        slice_errors[k] = std::current_exception();
      }
    };
    std::vector<std::thread> slice_threads;
    size_t started = 1; // slice 0 is run by this thread
    for(; started < nslices; ++started) {
      try {
        const size_t k = started;
        const std::shared_ptr<ibis::util::ThreadBudget> budget = lease.budget();
        slice_threads.emplace_back([&run_slice, budget, k]() {
          ibis::util::ThreadBudgetScope budget_scope(budget);
          run_slice(k);
        });
      } catch(...) {
        break;
      }
    }
    run_slice(0);
    for(size_t k = started; k < nslices; ++k) {
      run_slice(k); // the slices that could not get a thread
    }
    for(auto &t : slice_threads) {
      t.join();
    }
    for(size_t k = 0; k < nslices; ++k) {
      if( slice_errors[k] ) {
        std::rethrow_exception(slice_errors[k]);
      }
      matching_dim_rowids->insert(slice_dim_rows[k].begin(), slice_dim_rows[k].end());
      std::vector<uint64_t>().swap(slice_dim_rows[k]);
    }
    // free up columnar values
    column_vals_guard.reset();

    dimension_merge_mutex->lock();
    ++(*running_dimension_merges);
    dimension_merge_mutex->unlock();

    //deletes matching_dim_rowids
    try {
      std::thread(merge_dimension_keys, filter_it, matching_dim_rowids, running_dimension_merges, dimension_merge_mutex).detach();
      matching_dim_rowids_guard.release();
    } catch(...) {
      dimension_merge_mutex->lock();
      --(*running_dimension_merges);
      dimension_merge_mutex->unlock();
      throw;
    }

    #if 0
    filter_it->first->mtx.lock();
              
    for(auto insert_it = matching_dim_rowids.begin(); insert_it != matching_dim_rowids.end(); ++insert_it) {
      filter_it->first->add_matching_rownum(*insert_it);
    }
    
    filter_it->first->mtx.unlock();    
    #endif

  }
  
  if( match_count.size() > 0 ) {
    
    auto tmp = std::string((*part_it)->currentDataDir());
    auto find_it = matching_ridset->find(tmp);
    assert(find_it != matching_ridset->end());

    /* the row numbers are in ascending order, so the rows are fetched
       in the order they are stored */
    auto filtered_matching_ids = new std::vector<uint32_t>;
    const uint8_t all_filters = fact_table_filters->size();
    for(size_t i = 0; i < match_count.size(); ++i) {
      if( match_count[i] == all_filters ) {
        filtered_matching_ids->push_back(i + 1);
      }
    }
    
    if(filtered_matching_ids->size() > 0) {
      find_it->second = filtered_matching_ids;
    } else {
      delete filtered_matching_ids;
      find_it->second = NULL;
    }
  }
  } catch(const std::exception &e) {
    sql_print_error("WARP: join worker failed: %s", e.what());
    *join_error = 1;
  } catch(const char *msg) {
    sql_print_error("WARP: join worker failed: %s", msg);
    *join_error = 1;
  } catch(...) {
    sql_print_error("WARP: join worker failed with an unknown exception");
    *join_error = 1;
  }

  /* The column files read for this partition stay in the FastBit cache
     after the worker is done.  FastBit does not evict idle files to make
     room for the arrays that queries allocate, so cached files of
     finished partitions would fill the cache (about 16 MB per partition
     here) and make later workers fail.  Nothing in this partition is
     needed in memory until it is scanned. */
  try {
    warp_release_partition((*part_it)->currentDataDir());
  } catch(...) {
    // the files are just not released
  }

  parallel_join_mutex->lock();
  (*running_join_threads)--;
  parallel_join_mutex->unlock();

  delete column_query;
}

/* select() on a FastBit table returns a nil pointer when it fails, for
   example because the FastBit cache is too small for the selected data. */
static void warp_report_select_failure(const char *table_dir) {
  sql_print_error("WARP: could not read the selected rows of %s.  The FastBit cache "
                  "(warp_cache_size = %llu bytes) may be too small for this query.",
                  table_dir, (unsigned long long)my_cache_size);
}

/* Wait for all the join worker threads and dimension merges scheduled by
   this handler.  The threads use members of this handler, so it must not
   continue (or be destroyed) while they are running. */
void ha_warp::wait_for_join_threads() {
  for(;;) {
    parallel_join_mutex.lock();
    bool jobs_running = (running_join_threads != 0);
    parallel_join_mutex.unlock();
    dimension_merge_mutex.lock();
    bool merges_running = (running_dimension_merges != 0);
    dimension_merge_mutex.unlock();
    if(!jobs_running && !merges_running) break;
    struct timespec sleep_time = {0, 10000000L}; // 10 ms
    nanosleep(&sleep_time, NULL);
  }
}

/* FastBit reports resource problems (typically the file cache being too
   small for the data a query needs) by throwing exceptions.  They must not
   escape into the server, which would terminate it. */
int ha_warp::rnd_next(uchar *buf) {
  DBUG_ENTER("ha_warp::rnd_next");
  if(star_agg) {
    DBUG_RETURN(star_agg_next(buf));
  }
  const char* what = NULL;
  std::string detail;
  try {
    DBUG_RETURN(rnd_next_impl(buf));
  } catch(const std::bad_alloc &) {
    what = "out of memory";
  } catch(const std::exception &e) {
    detail = e.what();
    what = detail.c_str();
  } catch(const char *msg) {
    what = msg;
  } catch(...) {
    what = "unknown exception";
  }
  join_error = 1;
  wait_for_join_threads();
  sql_print_error("WARP: scan of %s failed: %s.  The FastBit cache (warp_cache_size = %llu bytes) "
                  "may be too small for this query.", share->data_dir_name, what,
                  (unsigned long long)my_cache_size);
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

int ha_warp::rnd_next_impl(uchar *buf) {
  DBUG_ENTER("ha_warp::rnd_next_impl");
  
  /* The thread budget of the statement is only needed when a partition is
     opened or joins are scheduled, not for every row that is returned, so
     it is installed when it is first needed in this call. */
  /* the join workers read the partitions of the table in their threads,
     the lock is held from when they are scheduled until they are done */
  std::optional<warp_table_read_lock> join_read_lock;
  std::optional<warp_index_build_scope> index_build_scope;
  auto use_thread_budget = [&]() {
    if(!index_build_scope) {
      index_build_scope.emplace(ha_thd(), thread_budget);
    }
  };

  // transaction id of the current row
  uint64_t row_trx_id = 0;
fetch_again:  
  
  if( !full_partition_scan && partitions != NULL && bitmap_merge_join_executed == false ) {
    use_thread_budget();
    join_read_lock.emplace(share->data_dir_name);
    if( std::string((*part_it)->currentDataDir()) == std::string(share->data_dir_name) ) {
      ++part_it;
    }
  
    while( part_it != partitions->end() ) {        
      /* the list of partitions also contains the (empty) top level
         directory of the table.  There is nothing to evaluate or join
         in a partition without rows. */
      if( (*part_it)->nRows() == 0 ) {
        ++part_it;
        continue;
      }
      //verify that the partition is valid / not empty
      base_table = ibis::table::create((*part_it)->currentDataDir());
      rownum = 0;
      if(!base_table) {
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      }
      
      delete base_table;
      base_table = NULL;
      filtered_table = NULL;
      
      if( join_error.load() != 0 ) {
        break; // stop scheduling, a worker failed
      }

      std::unique_ptr<ibis::query> column_query(
          new ibis::query((const char*)(0), (*part_it), (const char*)(0)));
      if( push_where_clause == "" ) {
        push_where_clause = "1=1";
      }
      
      column_query->addConditions(push_where_clause.c_str());
      if( column_query->evaluate() < 0 ) {
        sql_print_error("WARP: could not evaluate the pushed down condition on %s",
                        (*part_it)->currentDataDir());
        join_error = 1;
        break;
      }
      if( column_query->getNumHits() != 0 ) {
        // nothing happens if this function is called more than once during query evaluation
        // but it must be executed at least once when parallel hash join is being used
        bitmap_merge_join();
        
        // this is zero if join optimization is not being used
        if(fact_table_filters.size() == 0) {
        
          if( push_where_clause == "" ) {
            push_where_clause = "1=1";
          }
          
        } else {
        
          if( matching_ridset.size() == 0 ) {
            for(auto part_it2 = partitions->begin();part_it2 != partitions->end();++part_it2) {
              matching_ridset.emplace(std::make_pair(std::string((*part_it2)->currentDataDir()),(std::vector<uint32_t>*)NULL));
            }
          }  
          
          /* Every worker reads a column of its partition into memory (the
             column file and an int64 array of its values).  The workers
             share the FastBit cache, so the number of concurrent workers
             is limited by what the cache can hold. */
          const uint64_t worker_bytes =
              std::max<uint64_t>((*part_it)->nRows(), 1) * sizeof(int64_t) * 3;
          const uint64_t workers_by_cache =
              std::max<uint64_t>(1, (ibis::fileManager::currentCacheSize() / 2) / worker_bytes);
          /* The number of workers is limited by the thread budget of the
             statement (every worker takes a token, the connection thread
             is free) and by what the cache can hold.  A worker is always
             allowed when none is running, otherwise nothing would run
             when the degree of parallelism is 1. */
          const uint64_t max_workers = workers_by_cache;
          std::shared_ptr<ibis::util::ThreadBudget> join_budget =
              ibis::util::ThreadBudget::current();

          while(1) {
            if( join_error.load() != 0 ) {
              break;
            }
            parallel_join_mutex.lock();
            auto tmp = running_join_threads;
            parallel_join_mutex.unlock();
            
            bool wait_for_worker =
              (tmp >= max_workers ||
               (tmp > 0 && ibis::fileManager::bytesFree() < worker_bytes * 2));
            unsigned token = 0;
            if( !wait_for_worker && tmp > 0 ) {
              token = join_budget ? join_budget->tryAcquire(1) : 0;
              wait_for_worker = (token == 0);
            }
            if( wait_for_worker ) {
              struct timespec sleep_time;
              struct timespec remaining_time;
              sleep_time.tv_sec = (time_t)0;
              sleep_time.tv_nsec = 1000000L; // sleep a millisecond
              
              nanosleep(&sleep_time, &remaining_time);
              continue;
            } 
            
            parallel_join_mutex.lock();
            ++running_join_threads;
            
            parallel_join_mutex.unlock();
            try {
              ibis::query* cq = column_query.get();
              ibis::partList::iterator pit = part_it;
              fact_table_filter* ftf = &fact_table_filters;
              auto* mrs = &matching_ridset;
              auto* rjt = &running_join_threads;
              auto* pjm = &parallel_join_mutex;
              auto* rdm = &running_dimension_merges;
              auto* dmm = &dimension_merge_mutex;
              auto* jerr = &join_error;
              const size_t min_rows = THDVAR(table->in_use, parallel_min_rows);
              std::thread([=]() {
                /* the worker works under the budget of the statement, the
                   token is given back when it is done */
                ibis::util::ThreadBudgetScope budget_scope(join_budget);
                exec_pushdown_join(cq, pit, ftf, mrs, rjt, pjm, rdm, dmm, jerr, min_rows);
                if( token > 0 ) join_budget->release(token);
              }).detach();
              column_query.release(); // owned by the thread now
            } catch(...) {
              parallel_join_mutex.lock();
              --running_join_threads;
              parallel_join_mutex.unlock();
              if( token > 0 ) join_budget->release(token);
              throw;
            }
            break;

          }  
        }
      } 
      ++part_it;
      
    } // jobs for joining all the partitions involved in the query have been scheduled   
    
    part_it = partitions->begin();
    current_matching_ridset = NULL;
  } 
  
  // wait for scheduled join to complete
  while(all_jobs_completed == false) {
    parallel_join_mutex.lock();
    if( running_join_threads == 0 ) {
      parallel_join_mutex.unlock();
      // next time the mutex won't be held!
      all_jobs_completed = true;
      rownum = 0;
      break;
    }
    
    struct timespec sleep_time;
    struct timespec remaining_time;
    sleep_time.tv_sec = (time_t)0;
    sleep_time.tv_nsec = 1000000L; // sleep a millisecond
    parallel_join_mutex.unlock();
    nanosleep(&sleep_time, &remaining_time);
  }

  // there may be dimension merging still going on!
  while(all_dimension_merges_completed == false) {
    dimension_merge_mutex.lock();
    if( running_dimension_merges == 0 ) {
      dimension_merge_mutex.unlock();
      all_dimension_merges_completed = true;  
      continue;
    }
    dimension_merge_mutex.unlock();
    
    struct timespec sleep_time;
    struct timespec remaining_time;
    sleep_time.tv_sec = (time_t)0;
    sleep_time.tv_nsec = 1000000L; // sleep a millisecond
    nanosleep(&sleep_time, &remaining_time);
  }

  /* the workers are done with the files */
  join_read_lock.reset();

  /* A failed worker or merge means some partitions were not (completely)
     filtered.  Returning rows from here would give wrong results. */
  {
    bool filter_failed = false;
    for(auto &filter : fact_table_filters) {
      if(filter.first->failed.load()) filter_failed = true;
    }
    if(join_error.load() != 0 || filter_failed) {
      sql_print_error("WARP: a join worker failed on %s.  The FastBit cache (warp_cache_size = %llu bytes) "
                      "may be too small for this query.", share->data_dir_name,
                      (unsigned long long)my_cache_size);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }
  
  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  
  next_ridset:

  if( matching_ridset.size() > 0 ) {
    if(part_it == partitions->end()) {
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    
    if( current_matching_ridset == NULL ) {
      use_thread_budget();
      auto find_it = matching_ridset.find(std::string((*part_it)->currentDataDir()));
      
      if( find_it == matching_ridset.end() ) {
        ++part_it;
        goto next_ridset;
      }
      
      if( find_it->second == NULL ) {
        ++part_it;
        goto next_ridset;
      } 
    
      if(base_table != NULL) {
        delete cursor;
        cursor = NULL;
        delete filtered_table;
        filtered_table = NULL;
        delete base_table;
        base_table = NULL;
      }
    
      current_matching_ridset = find_it->second;
      current_matching_ridset_it = find_it->second->begin();
      
      if( !take_prefetched(find_it->first, &base_table, &filtered_table) ||
          filtered_table == NULL ) {
        /* not read ahead, or the read ahead did not work (for example
           because the cache was busy): read it here */
        warp_table_read_lock data_lock(share->data_dir_name);
        delete base_table;
        base_table = ibis::table::create(find_it->first.c_str());
        assert(base_table != NULL);
            
        // this will do some IO to read in projected columns that where not used for filters
        maintain_indexes(find_it->first.c_str());
        filtered_table = base_table->select(column_set.c_str(), push_where_clause.c_str());
        release_scanned_table();
      }
      prefetch_following_partitions();
      
      if(!filtered_table) {
        /* FastBit returns an empty table when no rows match, a nil pointer
           means the selection failed (usually: not enough cache).  Skipping
           the partition would silently drop its rows. */
        warp_report_select_failure(share->data_dir_name);
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      
      cursor = filtered_table->createCursor();

    }
    
  } else {
    
    // table scan (possibly with filters) without any joins
    if(cursor == NULL) {
      use_thread_budget();
      if( std::string((*part_it)->currentDataDir()) == std::string(share->data_dir_name) ) {
      
        ++part_it;
        if(part_it == partitions->end()) {
          DBUG_RETURN(HA_ERR_END_OF_FILE);
        }
      }
      
      if( !take_prefetched(std::string((*part_it)->currentDataDir()), &base_table, &filtered_table) ||
          filtered_table == NULL ) {
        warp_table_read_lock data_lock(share->data_dir_name);
        delete base_table;
        base_table = ibis::table::create((*part_it)->currentDataDir());
        assert(base_table != NULL);
      
        maintain_indexes((*part_it)->currentDataDir());
        filtered_table = base_table->select(column_set.c_str(), push_where_clause.c_str());
        release_scanned_table();
      }
      prefetch_following_partitions();
      
      if(filtered_table==NULL) {
        warp_report_select_failure(share->data_dir_name);
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      
      cursor = filtered_table->createCursor();  
      rownum = 0;
    } 
  }
  
  if( !cursor ) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  // will remain 10 if we hit the end of current_matching_ridset
  // otherwise is the result of the fetch.  if there is no 
  // current_matching_ridset the next row is fetched and if the
  // end of the resultset is reached, res will end up non-zero 
  int res = 10;
  if(matching_ridset.size() >0 ) {
    
    if(current_matching_ridset_it != current_matching_ridset->end()) {
      rownum = (*current_matching_ridset_it);
      res = cursor->fetch(rownum-1);
      ++current_matching_ridset_it;
    } 
    // if end of ridset res still = 10 here and the fetch failure
    // is handled below, objects are free'd etc..
  } else {
    
    // during pushdown joins the dimensions have a set of buffered rowids
    // this is a scan of one of the dimension tables (because current_matching_ridset_it )
    if( current_matching_dim_ridset != NULL ) {  
      if( current_matching_dim_ridset_it != current_matching_dim_ridset->end() ) {
        rownum = *current_matching_dim_ridset_it;
        res = cursor->fetch((*current_matching_dim_ridset_it)-1);
        ++current_matching_dim_ridset_it;
      } else {
        res = -1;
      }
    } else {
      res = cursor->fetch();
      ++rownum;
    }
  }
  
  if( res != 0 ) {
    fetch_count = 0;
    if( partitions != NULL && fact_table_filters.size() > 0 ) {
      // free up the memory used for buffering the matching rowids
      if( current_matching_ridset != NULL ) {
        delete current_matching_ridset;
        auto find_it = matching_ridset.find(std::string((*part_it)->currentDataDir()));
        find_it->second = NULL;
        current_matching_ridset = NULL;
      }
      
      // move to the next partition
      delete cursor;
      cursor = NULL;
      delete filtered_table;
      filtered_table = NULL;
      delete base_table;
      base_table = NULL;
      warp_release_partition((*part_it)->currentDataDir());
      ++part_it;
      goto next_ridset;
    }
       
    if(current_matching_dim_ridset != NULL) {
      /* This set is the dim_rownums member of a warp_filter_info owned by
         the fact table's handler, which frees it at the end of the
         statement (cleanup_pushdown_info).  Deleting it here would free
         the whole filter object, as the set is its first member. */
      current_matching_dim_ridset = NULL;
    }
    
    if(partitions != NULL) {
      const std::string finished_partition((*part_it)->currentDataDir());
      ++part_it;
      if(part_it == partitions->end()) {
        warp_release_partition(finished_partition.c_str());
        DBUG_RETURN(HA_ERR_END_OF_FILE); 
      }
      delete cursor;
      cursor = NULL;
      delete filtered_table;
      filtered_table=NULL;
      delete base_table;
      base_table=NULL;
      warp_release_partition(finished_partition.c_str());
      
      goto fetch_again;
    }
       
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }  
  
  ++fetch_count;
  
  cursor->getColumnAsULong("r", current_rowid);
  cursor->getColumnAsULong("t", row_trx_id);
  
  /* This sets is_trx_visible handler variable!
     If we already checked this trx_id in the last iteration
     then it does not have to be checked again and the
     is_trx_visible variable does not change. This function
     also sets last_trx_id to the transaction being c
     checked if the value is not the same as this 
     transaction.
  */

  
  is_trx_visible_to_read(row_trx_id);
  
  if(!is_trx_visible) {
    goto fetch_again;
  }
  
  // if the row would be visible due to row_trx_id it might not
  // be visible if it has been changed in a future transaction.
  // because the delete_rows bitmap has bits possibly committed
  // from future transaction, a history lock is created to 
  // maintain row visiblity
  if(!is_row_visible_to_read(current_rowid)) {
    goto fetch_again;
  }
  
  

  // Lock rows during a read if requested
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  int lock_taken = 0;
  if( lock_in_share_mode ) {
    lock_taken = warp_state->create_lock(current_rowid, current_trx, LOCK_SH);
    // row is exclusive locked so it has been deleted but this row should 
    // have already been skipped because it has a history lock
    if( lock_taken == LOCK_EX ) {
      goto fetch_again;
    }
  
    if( lock_taken != LOCK_SH && lock_taken != WRITE_INTENTION ) {
        // some sort of error happened like DEADLOCK or LOCK_WAIT_TIMEOUt
      DBUG_RETURN(lock_taken);
    }
  } else {
    if( lock_for_update ) {
      lock_taken = warp_state->create_lock(current_rowid, current_trx, WRITE_INTENTION);
      if( lock_taken != WRITE_INTENTION ) {
        DBUG_RETURN(lock_taken);
      }
    }  
  }
  find_current_row(buf, cursor);

  DBUG_RETURN(0);
}

/* Counts the rows that a scan of the table would return, without building a
   row for MySQL for each of them: only the transaction id column (and the row
   id column, if rows could have been deleted) is read and the same visibility
   rules as in rnd_next are applied to every row.  Returns false if the rows
   can not be counted this way, the caller then scans the table. */
bool ha_warp::count_visible_rows(ha_rows *num_rows) {
  /* a scan that locks rows, a partition filter or a pushed down condition
     or join changes what a scan returns */
  if(lock_in_share_mode || lock_for_update) {
    return false;
  }
  const char *partition_filter = THDVAR(table->in_use, partition_filter);
  if(partition_filter != NULL && *partition_filter != 0) {
    return false;
  }
  if(push_where_clause != "" && push_where_clause != "1=1") {
    return false;
  }
  auto pushdown_info = get_pushdown_info(table->in_use, table->alias);
  if(pushdown_info != NULL &&
     (pushdown_info->base_table != NULL || !pushdown_info->join_info.empty())) {
    return false;
  }

  /* no row is written while the columns are read */
  warp_table_read_lock data_lock(share->data_dir_name);

  /* the rows are not visible for every transaction, and the cached answer
     of is_trx_visible_to_read must not come from an earlier scan */
  last_trx_id = 0;

  /* if no row was ever deleted or updated, every row of a visible
     transaction is visible and the row ids are not needed */
  const bool check_rowids =
      warp_state->has_history_locks() ||
      warp_state->delete_bitmap->may_have_bits();

  ibis::partList parts;
  ibis::util::gatherParts(parts, share->data_dir_name, true);
  ha_rows total = 0;
  bool ok = true;
  for(auto it = parts.begin(); it != parts.end() && ok; ++it) {
    ibis::part *part = *it;
    if(part == NULL || part->nRows() == 0 ||
       std::string(part->currentDataDir()) == std::string(share->data_dir_name)) {
      continue; /* the top level directory has no rows */
    }
    const ibis::column *tcol = part->getColumn("t");
    const ibis::column *rcol = part->getColumn("r");
    ibis::array_t<uint64_t> trx_ids;
    ibis::array_t<uint64_t> row_ids;
    if(tcol == NULL || rcol == NULL ||
       tcol->getValuesArray(&trx_ids) != 0 || trx_ids.size() != part->nRows() ||
       (check_rowids &&
        (rcol->getValuesArray(&row_ids) != 0 || row_ids.size() != part->nRows()))) {
      ok = false;
      break;
    }
    const size_t nrows = trx_ids.size();
    uint64_t previous_trx = 0;
    bool visible = false;
    bool have_previous = false;
    for(size_t i = 0; i < nrows; ++i) {
      const uint64_t trx_id = trx_ids[i];
      if(!have_previous || trx_id != previous_trx) {
        previous_trx = trx_id;
        have_previous = true;
        visible = is_trx_visible_to_read(trx_id);
      }
      if(!visible) {
        continue;
      }
      if(check_rowids) {
        /* is_row_visible_to_read looks at current_rowid */
        current_rowid = row_ids[i];
        if(!is_row_visible_to_read(current_rowid)) {
          continue;
        }
      }
      ++total;
    }
    trx_ids = ibis::array_t<uint64_t>();
    row_ids = ibis::array_t<uint64_t>();
    warp_release_partition(part->currentDataDir());
  }
  for(auto it = parts.begin(); it != parts.end(); ++it) {
    delete *it;
    *it = NULL;
  }
  if(ok) {
    *num_rows = total;
  }
  return ok;
}

/* Called for SELECT COUNT(*) without a condition.  The generic version scans
   the table and builds every row; this counts the visible rows directly. */
int ha_warp::records(ha_rows *num_rows) {
  try {
    if(count_visible_rows(num_rows)) {
      return 0;
    }
  } catch(...) {
    /* out of memory in FastBit, the scan reports it the usual way */
  }
  return handler::records(num_rows);
}

/*
  Called after each table scan.
*/
int ha_warp::rnd_end() {
  DBUG_ENTER("ha_warp::rnd_end");
  DBUG_RETURN(end_scan(true));
}

/* Ends a scan.  An index lookup that is followed by another one ends its
   scan without finishing: what the statement wrote stays in the writer. */
int ha_warp::end_scan(bool finish) {
  DBUG_ENTER("ha_warp::end_scan");

  /* workers use members of this handler (and the part objects freed below) */
  wait_for_join_threads();
  discard_prefetched();
  join_error = 0;
  
  blobroot.Clear();
   
  push_where_clause = "";

  if(!scan_uses_pushdown_tables) {
    if(cursor) delete cursor;
    if(filtered_table) delete filtered_table;
    if(base_table) delete base_table;
  }
  scan_uses_pushdown_tables = false;

  if(partitions) {
    /* The partitions the scan went through were released one by one as it
       left them.  Releasing them all here would empty the cache whenever it
       is fairly full (the list holds the table directory too, which
       contains all the partitions), and the next query would read and
       decompress everything again. */
    for(auto it=partitions->begin();it!=partitions->end();++it) {
      delete *it;
      *it=NULL;
    } 
    delete partitions;
  }
  
  int write_rc = 0;
  if(finish) {
    /* Only a statement that wrote rows changes the files on disk.  A scan
       that only read must leave the files it cached for the next query. */
    if(writer != NULL && writer->mRows() > 0) {
      write_rc = write_buffered_rows_to_disk();
      ibis::fileManager::instance().flushDir(share->data_dir_name);
    }
  }
  index_scan_open = false;

  base_table = NULL;
  filtered_table = NULL;
  cursor = NULL;
  partitions = NULL;
  // these have to be reset for consecutive execution of queries on this 
  // THD / handle to continue working properly (ie not crash)
  matching_ridset.clear();
  for(auto &filter : fact_table_filters) {
    retired_fact_table_filters.push_back(filter);
  }
  fact_table_filters.clear();
  all_dimension_merges_completed = false;
  all_jobs_completed = false;
  current_matching_ridset = NULL;
  current_matching_dim_ridset = NULL;
  buffer.length(0);
  DBUG_RETURN(write_rc);
}

/*
  This records the current position *in the active cursor* for the current row.
  This is a logical reference to the row which doesn't have any meaning outside
  of this scan because scans will have different row numbers when the pushed
  conditions are different.

  For similar reasons, deletions must mark the physical rowid of the row in the
  deleted RID map.
*/
void ha_warp::position(const uchar *) {
  DBUG_ENTER("ha_warp::position");
  my_store_ptr(ref, ref_length, current_rowid);
  DBUG_VOID_RETURN;
}

/*
  Used to seek to a logical posiion stored with ::position().
*/
int ha_warp::rnd_pos(uchar *buf, uchar *pos) {
  int rc;
  DBUG_ENTER("ha_warp::rnd_pos");
  warp_index_build_scope index_build_scope(ha_thd(), thread_budget);
  
  ha_statistic_increment(&System_status_var::ha_read_rnd_count);
  current_rowid = my_get_ptr(pos, ref_length);
  /* rnd_pos can be used without a preceding table scan in this handler */
  if(column_set.empty()) {
    set_column_set();
  }
  rc = HA_ERR_KEY_NOT_FOUND;
  warp_table_read_lock data_lock(share->data_dir_name);
  base_table = ibis::mensa::create(share->data_dir_name);
  if(base_table != NULL) {
    filtered_table = base_table->select(column_set.c_str(), ("r=" + std::to_string(current_rowid)).c_str());
    if(filtered_table == NULL) {
      warp_report_select_failure(share->data_dir_name);
      rc = HA_ERR_OUT_OF_MEM;
    }
  }
  if(filtered_table != NULL && filtered_table->nRows() > 0) {
    cursor = filtered_table->createCursor();
    /* position the cursor on the (only) matching row before reading it */
    if(cursor != NULL && cursor->fetch() == 0) {
      rc = find_current_row(buf, cursor);
    }
  }

  delete cursor;
  delete filtered_table;
  delete base_table;
  cursor = NULL;
  filtered_table = NULL;
  base_table = NULL;
  DBUG_RETURN(rc);
}



/* ---------------------------------------------------------------------
   Indexes

   An index of a WARP table is a declaration, the data has no structure for
   it (FastBit builds its bitmap indexes of the columns itself).  A lookup
   through an index is a scan of the table with a condition that is made from
   the key.  The condition is added to the one that ECP pushed
   for the statement (push_where_clause), and FastBit evaluates it with its
   bitmap indexes.  The scan is the normal one (partitions, row
   visibility, locks, read ahead), so the rows are not returned in the order
   of the key and the indexes do not claim HA_READ_ORDER.
   --------------------------------------------------------------------- */

/* The column types that can be used in an index: the ones for which a key
   can be turned into an exact FastBit condition. */
static bool warp_key_part_supported(const KEY_PART_INFO *kp) {
  const Field *f = kp->field;
  if(f->is_virtual_gcol()) {
    return false;
  }
  /* no prefix keys */
  if(kp->key_part_flag & HA_PART_KEY_SEG) {
    return false;
  }
  switch(f->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
      return true;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
      if(kp->length < f->field_length) {
        return false;
      }
      /* utf8mb4_bin, which FastBit compares in the same way (PAD SPACE) */
      return f->charset()->number == 46;
    default:
      return false;
  }
}

/* The value of a key part, as it is written in a FastBit condition */
struct warp_key_value {
  bool is_null = false;
  std::string text;
};

/* @param ptr the key part in the key buffer (with the NULL byte if any) */
static bool warp_decode_key_part(TABLE *table, const KEY_PART_INFO *kp,
                                 const uchar *ptr, warp_key_value &v) {
  Field *f = kp->field;
  v.is_null = false;
  v.text.clear();
  if(kp->null_bit) {
    v.is_null = *ptr != 0;
    ++ptr;
    if(v.is_null) {
      return true;
    }
  }
  /* the key image is read through the field, in a record of its own so that
     the row buffers of the table are not touched */
  std::vector<uchar> scratch(table->s->reclength + 8, 0);
  const ptrdiff_t diff = scratch.data() - table->record[0];
  my_bitmap_map *org_bitmap = dbug_tmp_use_all_columns(table, table->read_set);
  f->move_field_offset(diff);
  f->set_key_image(ptr, kp->length);
  bool ok = true;
  if(f->real_type() == MYSQL_TYPE_VARCHAR ||
     f->real_type() == MYSQL_TYPE_STRING) {
    String tmp;
    String *val = f->val_str(&tmp);
    if(val == NULL) {
      ok = false;
    } else {
      std::string escaped;
      for(size_t i = 0; ok && i < val->length(); ++i) {
        const char c = val->ptr()[i];
        if(c == 0) {
          ok = false; /* FastBit strings end at a NUL */
        } else if(c == '\'') {
          escaped += "\\'";
        } else if(c == '\\') {
          escaped += "\\\\";
        } else {
          escaped += c;
        }
      }
      v.text = "'" + escaped + "'";
    }
  } else if(f->real_type() == MYSQL_TYPE_YEAR) {
    v.text = std::to_string(f->val_int());
  } else if(f->real_type() == MYSQL_TYPE_DATE ||
            f->real_type() == MYSQL_TYPE_NEWDATE ||
            f->real_type() == MYSQL_TYPE_DATETIME ||
            f->real_type() == MYSQL_TYPE_DATETIME2 ||
            f->real_type() == MYSQL_TYPE_TIMESTAMP ||
            f->real_type() == MYSQL_TYPE_TIMESTAMP2 ||
            warp_is_time_type(f->real_type())) {
    /* the same encoding as the one the rows are written with (see
       encode_quote): TIME as biased microseconds, the others as packed
       DATETIME values; a TIMESTAMP in UTC */
    if(warp_is_time_type(f->real_type())) {
      Time_val t;
      if(f->val_time(&t)) {
        ok = false;
      } else {
        v.text = std::to_string(warp_encode_time(t));
      }
    } else if(warp_is_timestamp_type(f->real_type())) {
      v.text = std::to_string(warp_timestamp_field_value(f));
    } else {
      Datetime_val dt;
      if(f->val_datetime(&dt, TIME_DATETIME_ONLY)) {
        ok = false;
      } else {
        v.text = std::to_string(TIME_to_longlong_datetime_packed(dt));
      }
    }
  } else {
    const longlong n = f->val_int();
    const bool is_unsigned = f->all_flags() & UNSIGNED_FLAG;
    if(f->real_type() == MYSQL_TYPE_LONGLONG) {
      /* exact for the values that a double can not represent */
      v.text = is_unsigned ? std::to_string((ulonglong)n) + "U"
                           : std::to_string(n) + "L";
    } else {
      v.text = std::to_string(n);
    }
  }
  f->move_field_offset(-diff);
  dbug_tmp_restore_column_map(table->read_set, org_bitmap);
  return ok;
}

/* The condition "column op value" for a part of a key.  NULL sorts before
   every value, like in the indexes of MySQL.  The columns are named by the
   position of the column in the table, the NULL markers of nullable columns
   are the columns n<position>. */
static std::string warp_key_part_condition(const Field *f,
                                           const warp_key_value &v,
                                           const std::string &op) {
  const std::string pos = std::to_string(f->field_index());
  const std::string null_marker = "n" + pos;
  if(v.is_null) {
    if(op == "=" || op == "<=") return null_marker + " = 1";
    if(op == ">=") return "1=1";
    if(op == ">") return null_marker + " = 0";
    return "1=0"; /* nothing is smaller than NULL */
  }
  const std::string cmp = "c" + pos + " " + op + " " + v.text;
  if(!f->is_nullable()) {
    return "(" + cmp + ")";
  }
  if(op == "<" || op == "<=") {
    return "(" + null_marker + " = 1 OR (" + null_marker + " = 0 AND " + cmp +
           "))";
  }
  return "(" + null_marker + " = 0 AND " + cmp + ")";
}

/* The rows that are after (lower) or before (not lower) a key, in the order
   of the key parts: the rows with a bigger first part, or with the same first
   part and a bigger second part and so on.  A key with fewer parts than the
   index is a prefix, the rest of the parts are not compared. */
static std::string warp_key_bound(const KEY *key,
                                  const std::vector<warp_key_value> &values,
                                  size_t part, bool lower, bool inclusive) {
  const Field *f = key->key_part[part].field;
  const std::string strict = lower ? ">" : "<";
  const std::string loose = lower ? ">=" : "<=";
  if(part + 1 == values.size()) {
    return warp_key_part_condition(f, values[part], inclusive ? loose : strict);
  }
  return "(" + warp_key_part_condition(f, values[part], strict) + " OR (" +
         warp_key_part_condition(f, values[part], "=") + " AND " +
         warp_key_bound(key, values, part + 1, lower, inclusive) + "))";
}

static bool warp_decode_key(TABLE *table, uint idx, const key_range *range,
                            std::vector<warp_key_value> &values) {
  const KEY *key = &table->key_info[idx];
  const uchar *ptr = range->key;
  values.clear();
  for(uint i = 0; i < key->user_defined_key_parts &&
                  (range->keypart_map & (key_part_map(1) << i)); ++i) {
    const KEY_PART_INFO *kp = &key->key_part[i];
    warp_key_value v;
    if(!warp_decode_key_part(table, kp, ptr, v)) {
      return false;
    }
    values.push_back(v);
    ptr += kp->store_length;
  }
  return !values.empty();
}

/* The condition for a lookup or for a range of the index.  An empty
   condition is every row.  Returns false for a lookup that can not be
   made (the search of the previous row or the last one of a key, for
   example). */
bool ha_warp::make_key_condition(const key_range *start_key,
                                 const key_range *end_key, bool eq_range,
                                 std::string &condition) {
  const KEY *key = &table->key_info[active_index];
  std::vector<warp_key_value> values;
  condition = "";

  if(start_key != NULL && start_key->flag == HA_READ_KEY_EXACT &&
     (end_key == NULL ? true : eq_range)) {
    /* the rows with this key (or this prefix of the key) */
    if(!warp_decode_key(table, active_index, start_key, values)) {
      return false;
    }
    for(size_t i = 0; i < values.size(); ++i) {
      if(i > 0) condition += " AND ";
      condition += warp_key_part_condition(key->key_part[i].field, values[i],
                                           "=");
    }
    return true;
  }

  if(start_key != NULL) {
    if(start_key->flag != HA_READ_KEY_EXACT &&
       start_key->flag != HA_READ_KEY_OR_NEXT &&
       start_key->flag != HA_READ_AFTER_KEY) {
      return false;
    }
    if(!warp_decode_key(table, active_index, start_key, values)) {
      return false;
    }
    condition = warp_key_bound(key, values, 0, true,
                               start_key->flag != HA_READ_AFTER_KEY);
  }
  if(end_key != NULL) {
    if(end_key->flag != HA_READ_AFTER_KEY &&
       end_key->flag != HA_READ_BEFORE_KEY &&
       end_key->flag != HA_READ_KEY_EXACT) {
      return false;
    }
    if(!warp_decode_key(table, active_index, end_key, values)) {
      return false;
    }
    if(condition != "") condition += " AND ";
    condition += warp_key_bound(key, values, 0, false,
                                end_key->flag != HA_READ_BEFORE_KEY);
  }
  return true;
}

/* Starts the scan for a lookup.  The scan of an earlier lookup is ended
   first, but the rows that this statement wrote stay in the buffer of the
   writer until the statement ends: they must not be found by its own scans. */
int ha_warp::start_index_scan(const std::string &key_condition) {
  if(index_scan_open) {
    end_scan(false);
  }
  std::string where = index_base_where;
  if(key_condition != "") {
    where = where == "" ? key_condition
                        : "(" + where + ") AND (" + key_condition + ")";
  }
  push_where_clause = where;
  index_scan_mode = true;
  int rc = rnd_init(true);
  index_scan_mode = false;
  /* the rows of the lookup are not joined with the filters of other tables */
  full_partition_scan = true;
  if(rc == 0) {
    index_scan_open = true;
  }
  return rc;
}

int ha_warp::index_init(uint idxno, bool) {
  DBUG_ENTER("ha_warp::index_init");
  /* the condition that ECP pushed for the statement */
  index_base_where = push_where_clause == "1=1" ? "" : push_where_clause;
  if(index_scan_open) {
    end_scan(false);
  }
  active_index = idxno;
  DBUG_RETURN(0);
}

int ha_warp::index_end() {
  DBUG_ENTER("ha_warp::index_end");
  int rc = 0;
  if(index_scan_open || writer != NULL) {
    rc = end_scan(true);
  }
  /* the next index_init of the statement needs it again */
  push_where_clause = index_base_where;
  DBUG_RETURN(rc);
}

int ha_warp::index_read_map(uchar *buf, const uchar *key,
                            key_part_map keypart_map,
                            enum ha_rkey_function find_flag) {
  DBUG_ENTER("ha_warp::index_read_map");
  ha_statistic_increment(&System_status_var::ha_read_key_count);
  key_range start = {key, 0, keypart_map, find_flag};
  std::string condition;
  if(find_flag != HA_READ_KEY_EXACT && find_flag != HA_READ_KEY_OR_NEXT &&
     find_flag != HA_READ_AFTER_KEY) {
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  if(!make_key_condition(&start, NULL, false, condition)) {
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  int rc = start_index_scan(condition);
  if(rc != 0) {
    DBUG_RETURN(rc);
  }
  DBUG_RETURN(rnd_next(buf));
}

/* Used for the tables that the optimizer reads once (constants), without
   index_init and index_end.  The scan stays open until the next one starts or
   the statement ends, the row may use memory of the scan. */
int ha_warp::index_read_idx_map(uchar *buf, uint idxno, const uchar *key,
                                key_part_map keypart_map,
                                enum ha_rkey_function find_flag) {
  DBUG_ENTER("ha_warp::index_read_idx_map");
  const std::string saved = push_where_clause;
  const std::string saved_base = index_base_where;
  const uint saved_idx = active_index;
  if(index_scan_open) {
    end_scan(false);
  }
  active_index = idxno;
  index_base_where = saved == "1=1" ? "" : saved;
  int rc = index_read_map(buf, key, keypart_map, find_flag);
  push_where_clause = saved;
  index_base_where = saved_base;
  active_index = saved_idx;
  DBUG_RETURN(rc);
}

int ha_warp::index_next(uchar *buf) {
  DBUG_ENTER("ha_warp::index_next");
  ha_statistic_increment(&System_status_var::ha_read_next_count);
  if(!index_scan_open) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  DBUG_RETURN(rnd_next(buf));
}

/* every row of the scan has the key */
int ha_warp::index_next_same(uchar *buf, const uchar *, uint) {
  return index_next(buf);
}

int ha_warp::index_first(uchar *buf) {
  DBUG_ENTER("ha_warp::index_first");
  ha_statistic_increment(&System_status_var::ha_read_first_count);
  int rc = start_index_scan("");
  if(rc != 0) {
    DBUG_RETURN(rc);
  }
  DBUG_RETURN(rnd_next(buf));
}

/* The rows of a range are found by one scan, which is not in the order of the
   key: the end of the range is part of its condition. */
int ha_warp::read_range_first(const key_range *start_key,
                              const key_range *end_key, bool eq_range, bool) {
  DBUG_ENTER("ha_warp::read_range_first");
  std::string condition;
  if(!make_key_condition(start_key, end_key, eq_range, condition)) {
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  ha_statistic_increment(&System_status_var::ha_read_key_count);
  int rc = start_index_scan(condition);
  if(rc != 0) {
    DBUG_RETURN(rc);
  }
  rc = rnd_next(table->record[0]);
  DBUG_RETURN(rc == HA_ERR_KEY_NOT_FOUND ? HA_ERR_END_OF_FILE : rc);
}

int ha_warp::read_range_next() {
  DBUG_ENTER("ha_warp::read_range_next");
  DBUG_RETURN(index_next(table->record[0]));
}

/* The number of rows of a range, as FastBit estimates it with the bitmap
   indexes (the upper bound, the rows in the delete bitmap and of
   transactions that are not visible are not known). */
ha_rows ha_warp::records_in_range(uint idxno, key_range *min_key,
                                  key_range *max_key) {
  const uint saved_idx = active_index;
  active_index = idxno;
  std::string condition;
  const bool ok = make_key_condition(
      min_key, max_key,
      min_key != NULL && max_key != NULL && min_key->flag == HA_READ_KEY_EXACT &&
          max_key->flag == HA_READ_AFTER_KEY,
      condition);
  active_index = saved_idx;
  if(!ok) {
    return HA_POS_ERROR;
  }
  std::string where = push_where_clause == "1=1" ? "" : push_where_clause;
  if(condition != "") {
    where = where == "" ? condition
                        : "(" + where + ") AND (" + condition + ")";
  }
  try {
    warp_table_read_lock data_lock(share->data_dir_name);
    std::unique_ptr<ibis::mensa> estimator(
        new ibis::mensa(share->data_dir_name));
    uint64_t rows = estimator->nRows();
    if(where != "") {
      uint64_t min = 0, max = 0;
      estimator->estimate(where.c_str(), min, max);
      rows = max;
    }
    return rows > 0 ? rows : 1;
  } catch(...) {
    return HA_POS_ERROR;
  }
}

/**
 * Push conditions to a single WARP table.
 *
 * @param table  The table being accessed.
 * @param filter The AccessPath::FILTER directly above the table access, or
 *               nullptr if there is none.
 * @param join   The JOIN for the query block.
 *
 * Whatever part of the FILTER condition can not be evaluated by WARP is left
 * in the FILTER.  If everything was pushed the FILTER is eliminated.
 */
static void warp_push_table_conditions(THD *thd, TABLE *table,
                                       AccessPath *filter, const JOIN *join) {
  // we can't do pushdown to non-WARP tables.
  ha_warp *const ha = dynamic_cast<ha_warp *>(table->file);
  if (ha == nullptr) return;

  /* The handler is reused by later statements.  A condition pushed down for
     an earlier statement whose scan did not finish (an error, for example)
     must not be applied to a statement that has no condition. */
  ha->push_where_clause = "";
  ha->star_agg.reset();

  const Item *cond = (filter != nullptr) ? filter->filter().condition : nullptr;
  if (cond == nullptr && join->where_cond == nullptr) return;

  auto share = ha->get_warp_share();
  auto pushdown_info =
      get_or_create_pushdown_info(thd, table->alias, share->data_dir_name);
  assert(pushdown_info != nullptr);

  ha->push_where_clause = "";
  const Item *remainder = nullptr;
  if (cond) {
    remainder = ha->cond_push(cond);
  }
  auto save_where = ha->push_where_clause;
  if (join->where_cond) {
    ha->push_where_clause = "";
    ha->cond_push(join->where_cond);
  }
  /* either part may be empty: nothing of it was pushed */
  if (ha->push_where_clause != "" && save_where != "") {
    ha->push_where_clause += " AND ";
  }
  ha->push_where_clause += save_where;
  pushdown_info->filter = ha->push_where_clause;

  if (filter == nullptr) return;

  // Update the FILTER with the conditions WARP did not take responsibility for.
  filter->filter().condition = const_cast<Item *>(remainder);

  // To get correct explain output: (Does NOT affect what is executed)
  // Need to set the QEP_TAB condition as well. Note that QEP_TABs
  // are not 'executed' any longer -> affects only explain output.
  // The Hypergraph-optimizer does not construct QEP_TABs.
  QEP_TAB *qep_tab = table->reginfo.qep_tab;
  if (qep_tab != nullptr) {
    qep_tab->set_condition(const_cast<Item *>(remainder));
    qep_tab->set_condition_optim();
  }

  if (remainder == nullptr) {
    // Entire FILTER condition was pushed down.  Remove the FILTER operation,
    // keep the estimated rows/cost (used for explain only).
    AccessPath *child = filter->filter().child;
    child->set_num_output_rows(filter->num_output_rows());
    child->set_cost(filter->cost());
    *filter = std::move(*child);
  }
}

/* True while warp_push_to_engine processes a query in which a WARP table is
   the inner table of a nested loop join.  WARP's join pushdown computes the
   join once and assumes every table is scanned exactly once, which is not
   true for the inner table of a nested loop (it is scanned again for every
   outer row), so join conditions between WARP tables are left to MySQL. */
static thread_local bool warp_inner_nested_loop_table = false;

/* Does the access path (sub)tree read from a WARP table? */
static bool warp_path_reads_warp_table(AccessPath *path, const JOIN *join) {
  bool found = false;
  WalkAccessPaths(path, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  [&found](AccessPath *subpath, const JOIN *) {
                    TABLE *table = GetBasicTable(subpath);
                    if (table != nullptr &&
                        dynamic_cast<ha_warp *>(table->file) != nullptr) {
                      found = true;
                    }
                    return found;
                  });
  return found;
}

/* Is a WARP table the inner (rescanned) side of a nested loop join? */
static bool warp_has_inner_nested_loop_table(AccessPath *root_path,
                                             const JOIN *join) {
  bool found = false;
  WalkAccessPaths(
      root_path, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
      [&found](AccessPath *subpath, const JOIN *join_arg) {
        AccessPath *inner = nullptr;
        if (subpath->type == AccessPath::NESTED_LOOP_JOIN) {
          inner = subpath->nested_loop_join().inner;
        } else if (subpath->type ==
                   AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL) {
          inner = subpath->nested_loop_semijoin_with_duplicate_removal().inner;
        } else if (subpath->type == AccessPath::BKA_JOIN) {
          inner = subpath->bka_join().inner;
        }
        if (inner != nullptr && warp_path_reads_warp_table(inner, join_arg)) {
          found = true;
        }
        return found;
      });
  return found;
}

#include "warp_star_agg.h"

/**
 * handlerton::push_to_engine implementation.  Walks the AccessPath tree and
 * offers the condition of each FILTER sitting on top of a WARP table access
 * (and the WHERE clause) to that table's handler (ha_warp::cond_push).
 *
 * @return Possible error code, '0' if no errors.
 */
int warp_push_to_engine(THD *thd, AccessPath *root_path, JOIN *join) {
  auto func = [thd](AccessPath *subpath, const JOIN *join_arg) {
    if (subpath->type == AccessPath::FILTER) {
      AccessPath *child = subpath->filter().child;
      TABLE *table = GetBasicTable(child);
      if (table != nullptr) {
        warp_push_table_conditions(thd, table, subpath, join_arg);
        return true;  // child is a plain table access, nothing more to walk
      }
      return false;
    }
    TABLE *table = GetBasicTable(subpath);
    if (table != nullptr) {
      warp_push_table_conditions(thd, table, nullptr, join_arg);
      return true;
    }
    return false;
  };
  warp_inner_nested_loop_table =
      warp_has_inner_nested_loop_table(root_path, join);
  WalkAccessPaths(root_path, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  func);
  warp_inner_nested_loop_table = false;
  /* the star joins that the engine can evaluate and aggregate itself */
  warp_try_star_aggregation(thd, root_path, join);
  return 0;
}

/* This is the ECP (engine condition pushdown) handler code.  This is where the
   WARP magic really happens from a MySQL standpoint, since it allows index
   usage that MySQL would not normally support and provides automatic indexing
   for filter conditions.

   This code is called from ha_warp::engine_push in 8.0.20+
*/
const Item *ha_warp::cond_push(const Item *cond) {
  /* This state belongs to the statement that is being planned, it must not be
     shared: every connection has a thread and pushes its conditions at the
     same time as the others.  With ordinary static variables the sessions
     added their conditions to one string, so one statement could be given the
     conditions of another one (or a half made condition such as "a = 1 AND "). */
  static thread_local int depth=0;
  static thread_local int unpushed_condition_count = 0;
  static thread_local int condition_count = 0;
  static thread_local std::string where_clause = "";

  // reset the variables when called at depth 0
  if(depth == 0) {
    condition_count = 0;
    unpushed_condition_count = 0;
    where_clause = "";
  }

  /* A simple comparison without conjuction or disjunction */
  if(cond->type() == Item::Type::FUNC_ITEM) {
    condition_count++;
    
    int rc = append_column_filter(cond, where_clause);
    if(rc != 1) {
      unpushed_condition_count++;
      where_clause += "1=1";
      return cond;
    }
    /* List of connected simple conditions */
  } else if(cond->type() == Item::Type::COND_ITEM) {
    auto item_cond = (dynamic_cast<Item_cond *>(const_cast<Item *>(cond)));
    List<Item> items = *(item_cond->argument_list());
    auto cnt = items.size();
    where_clause += "(";
    
    for (uint i = 0; i < cnt; ++i) {
      auto item = items.pop();
      condition_count++;
      if(i > 0) {
        if(item_cond->functype() == Item_func::Functype::COND_AND_FUNC) {
          where_clause += " AND ";
        } else if(item_cond->functype() == Item_func::Functype::COND_OR_FUNC) {
          where_clause += " OR ";
        } else {
          where_clause += "1=1";
          unpushed_condition_count++;
          /* not handled */
          return cond;
        }
      }
      /* recurse to print the field and other items.  This should be a
         FUNC_ITEM. if it isn't, then the item will be returned by this function
         and pushdown evaluation will be abandoned.
      */
      ++depth;
      
      if(cond_push(item) != NULL) {
        unpushed_condition_count++;
        //items->push_back(item);
      }
      //items->push_front(item);
      --depth;
    }
    
    where_clause += ")";
  } else {
    /* Any other item (a column used as a condition, a constant, a
       subquery...) is not pushed: the clause must stay valid and MySQL has to
       evaluate the item */
    condition_count++;
    unpushed_condition_count++;
    where_clause += "1=1";
    return cond;
  }
  
  // only push a where clause if there were condtiions that were actually pushed
  if(depth == 0 && (unpushed_condition_count != condition_count)){
    push_where_clause += where_clause;
  }
  
  if(unpushed_condition_count>0) {
     return cond;
  }
  return NULL;
}

/* return 1 if this clause could not be processed (will be processed by MySQL)*/
/* FastBit evaluates numeric comparisons in double precision, which is
   exact only for integers of magnitude below 2^53.  Larger constants are
   compared exactly through FastBit's 64-bit integer syntax (a value with
   an L or U suffix), which exists for =, !=, <, <=, >, >=, BETWEEN and
   IN lists. */
static constexpr uint64_t WARP_EXACT_DOUBLE_LIMIT = 1ULL << 53;

static bool warp_int_is_exact_double(const Item *item) {
  const longlong v = const_cast<Item *>(item)->val_int();
  if (item->unsigned_flag) return (ulonglong)v < WARP_EXACT_DOUBLE_LIMIT;
  return v > -(longlong)WARP_EXACT_DOUBLE_LIMIT &&
         v < (longlong)WARP_EXACT_DOUBLE_LIMIT;
}

/* Format an integer constant for a FastBit condition.  Returns false if the
   comparison can not be pushed down exactly.
   @param exact_syntax  write the value with FastBit's 64-bit integer suffix
   @param col_unsigned  the compared column is unsigned */
static bool warp_format_int(const Item *item, bool exact_syntax,
                            bool col_unsigned, std::string &out) {
  const longlong v = const_cast<Item *>(item)->val_int();
  const bool negative = !item->unsigned_flag && v < 0;
  if (!exact_syntax) {
    out = item->unsigned_flag ? std::to_string((ulonglong)v)
                              : std::to_string(v);
    return true;
  }
  if (col_unsigned) {
    if (negative) return false;
    out = std::to_string((ulonglong)v) + "U";
  } else {
    if (item->unsigned_flag && (ulonglong)v > (ulonglong)LLONG_MAX)
      return false;
    out = std::to_string(v) + "L";
  }
  return true;
}

/* Pushes a comparison of a string column with string constants (<, <=, >, >=
   and BETWEEN) down to FastBit.  The result must be the same as MySQL's:
   WARP strings are utf8mb4_bin, which MySQL compares byte by byte with the
   trailing spaces not counting (PAD SPACE), and FastBit compares the same
   way (ibis::util::padSpaceCompare).  A pushed condition is not evaluated
   by MySQL again, so the conditions that can not be pushed are left to it:
   other collations (an explicit COLLATE, for example), binary strings,
   constants that are not plain strings and strings with a NUL character.

   op is the operator of the comparison (not used for BETWEEN).  Returns true
   and appends the FastBit condition to out if the condition was pushed. */
static bool warp_push_string_range(Item_func *func, bool is_between,
                                   const std::string &op, std::string &out) {
  Item **args = func->arguments();
  const uint nargs = func->arg_count;
  const Item_field *col_item = NULL;
  std::string fb_op = op;
  const Item *consts[2] = {NULL, NULL};
  uint nconsts = 0;

  if(is_between) {
    if(nargs != 3 || args[0]->type() != Item::Type::FIELD_ITEM) {
      return false;
    }
    col_item = down_cast<Item_field *>(args[0]);
    consts[0] = args[1];
    consts[1] = args[2];
    nconsts = 2;
  } else {
    if(nargs != 2) {
      return false;
    }
    if(args[0]->type() == Item::Type::FIELD_ITEM) {
      col_item = down_cast<Item_field *>(args[0]);
      consts[0] = args[1];
    } else if(args[1]->type() == Item::Type::FIELD_ITEM) {
      /* constant OP column: the column goes first, so mirror the operator */
      col_item = down_cast<Item_field *>(args[1]);
      consts[0] = args[0];
      if(op == " < ") fb_op = " > ";
      else if(op == " <= ") fb_op = " >= ";
      else if(op == " > ") fb_op = " < ";
      else if(op == " >= ") fb_op = " <= ";
      else return false;
    } else {
      return false;
    }
    nconsts = 1;
  }

  const Field *fld = col_item->field;
  const enum_field_types rt = fld->real_type();
  if(rt != MYSQL_TYPE_VARCHAR && rt != MYSQL_TYPE_STRING &&
     rt != MYSQL_TYPE_VAR_STRING) {
    return false;
  }
  /* only the utf8mb4_bin collation of WARP columns, for the column and for
     the comparison */
  if(fld->charset()->number != 46) {
    return false;
  }
  const CHARSET_INFO *cmp_cs = func->compare_collation();
  if(cmp_cs == NULL || cmp_cs->number != 46) {
    return false;
  }

  std::string values[2];
  for(uint i = 0; i < nconsts; ++i) {
    if(consts[i]->type() != Item::Type::STRING_ITEM) {
      return false;
    }
    Item *citem = const_cast<Item *>(consts[i]);
    String s;
    String *val = citem->val_str(&s);
    if(val == NULL) {
      return false;
    }
    const char *ptr = val->ptr();
    bool ascii = true;
    for(size_t j = 0; j < val->length(); ++j) {
      const char c = ptr[j];
      if(c == 0) {
        return false; /* FastBit strings end at a NUL */
      }
      if(static_cast<unsigned char>(c) >= 0x80) {
        ascii = false;
      }
    }
    /* a constant in another character set would have to be converted */
    if(!ascii && strncmp(citem->collation.collation->csname, "utf8mb4", 7)) {
      return false;
    }
    std::string escaped;
    for(size_t j = 0; j < val->length(); ++j) {
      const char c = ptr[j];
      if(c == '\'') {
        escaped += "\\'";
      } else if(c == '\\') {
        escaped += "\\\\";
      } else {
        escaped += c;
      }
    }
    values[i] = "'" + escaped + "'";
  }

  const std::string field_index = std::to_string(fld->field_index());
  std::string cond;
  if(fld->is_nullable()) {
    cond = "(n" + field_index + " = 0 AND ";
  }
  cond += "c" + field_index;
  if(is_between) {
    cond += " BETWEEN " + values[0] + " AND " + values[1];
  } else {
    cond += fb_op + values[0];
  }
  if(fld->is_nullable()) {
    cond += ")";
  }
  out += cond;
  return true;
}

int ha_warp::append_column_filter(const Item *cond,
                                   std::string &where_clause) {
  bool field_may_be_null = false;
  bool is_between = false;
  bool is_in = false;
  bool is_is_null = false;
  bool is_isnot_null = false;
  bool is_eq = false;
  std::string build_where_clause = "";
  if(cond->type() == Item::Type::FUNC_ITEM) {

    Item_func *tmp = dynamic_cast<Item_func *>(const_cast<Item *>(cond));
    std::string op = "";

    /* There are only a small number of options currently available for
       filtering at the WARP SE level.  The basic numeric filters are presented
       here.
    */
    switch(tmp->functype()) {
      /* when op = " " there is special handling below because the
         syntax of the given function differs from the "regular"
         functions.
      */
      case Item_func::Functype::BETWEEN:
        is_between = true;
        break;

      case Item_func::Functype::IN_FUNC:
        is_in = true;
        break;

      case Item_func::Functype::ISNULL_FUNC:
        is_is_null = true;
        break;

      case Item_func::Functype::ISNOTNULL_FUNC:
        is_isnot_null = true;
        break;

      /* normal arg0 OP arg1 type operators */
      case Item_func::Functype::EQ_FUNC:
      case Item_func::Functype::EQUAL_FUNC:
        op = " = ";
        is_eq = true;
        break;

      case Item_func::Functype::LIKE_FUNC:
        op = " LIKE ";
        break;

      case Item_func::Functype::LT_FUNC:
        op = " < ";
        break;

      case Item_func::Functype::GT_FUNC:
        op = " > ";
        break;

      case Item_func::Functype::GE_FUNC:
        op = " >= ";
        break;

      case Item_func::Functype::LE_FUNC:
        op = " <= ";
        break;

      case Item_func::Functype::NE_FUNC:
        op = " != ";
        break;

      default:
        return 0;
    }

    Item **arg = tmp->arguments();
    //This is a fix for queries that have CONST filters on more than one table conjoined in an AND or an OR
    //when this happens, the field item will have a different alias from the table we are currently working
    //on (table->alias).  
    //For example, a TPC-H query contains the following:
    //and and ( (n1.n_name = 'JORDAN' and n2.n_name = 'BRAZIL') or (n1.n_name = 'BRAZIL' and n2.n_name = 'JORDAN') )                         
    //notice that there are AND conditions that compare constants in diffrent tables.
    for(size_t arg_num = 0; arg_num < tmp->arg_count-1; ++arg_num) {
      // if a field item refers to another field, then this is a join, and it is handled below in JOIN PUSHDOWN
      if( ( arg[arg_num]->type() == Item::Type::FIELD_ITEM && arg[arg_num+1]->type() != Item::Type::FIELD_ITEM ) )  {
        
        if(arg[arg_num]->used_tables() == 0) continue;
        
        auto str = ItemToString(arg[arg_num]);
        const char* dot_pos = strstr(str.c_str(), ".");
        const char* dot_pos2 = strstr(dot_pos+1, ".");
        std::string alias;
        //ssb.lineorder.LO_Quantity
        if(dot_pos2 != NULL) {
          alias=str.substr(dot_pos - str.c_str()+1, dot_pos2 - dot_pos -1);
        } else {
          alias=str.substr(0, dot_pos - str.c_str());
        }
        if(std::string(table->alias) != alias) {
          return 0;
        }
      }
    }
    /* IS NULL and IS NOT NULL have one argument, which the loop above does
       not look at: a column of another table is not a condition of this one */
    if(tmp->arg_count == 1 && arg[0]->type() == Item::Type::FIELD_ITEM) {
      Field *field = down_cast<Item_field *>(arg[0])->field;
      if(field != nullptr && field->table != table) {
        return 0;
      }
    }
    /* JOIN PUSHDOWN
       ***********************************************************
       This detects where two fields are compared to each other in
       different tables which is a join condition.  The pushdown
       information is retrieved for both tables and pushdown conditions
       are attached to the larger table.  Note that nothing is pushed
       down right now, this just computes the structures for it to
       happen when a scan is initiated.
    */
    
    if(tmp->arg_count == 2 && arg[0]->type() == Item::Type::FIELD_ITEM &&
        arg[0]->type() == arg[1]->type()
    ) {
      
      // only support equijoin right now
      if(!is_eq) {
        return 0;
      }      

      // the join pushdown does not support tables that are scanned more
      // than once (see warp_inner_nested_loop_table)
      if(warp_inner_nested_loop_table) {
        return 0;
      }
      
      Item_field* f0 = (Item_field *)(arg[0]);
      Item_field* f1 = (Item_field *)(arg[1]);

      /* a TIMESTAMP is stored in UTC, the other temporal types are not */
      if(warp_is_timestamp_type(f0->field->real_type()) !=
         warp_is_timestamp_type(f1->field->real_type())) {
        return 0;
      }

      // Get the pushdown information - something is quite broken if these are NULL
      auto f0_info = get_pushdown_info(table->in_use, f0->m_table_ref->alias);
      auto f1_info = get_pushdown_info(table->in_use, f1->m_table_ref->alias);
      
      if(f0_info == NULL || f1_info == NULL) {
        return 0;
      }
      
      bool this_is_dim_table = true;
      if(f1_info->datadir != share->data_dir_name) {
        this_is_dim_table = false; 
      }
      const char* dim_field_mysql_name;
      const char* fact_field_mysql_name;
      const char* dim_alias;
      //const char* fact_alias;
      
      // Used later translate the MySQL table column names into
      // WARP ordinal column names.  These are attched to the
      // join_info member in the pushdown structure.
      // join_info[fact_field] -> dim_info{dim_alias, dim_field}
      Field** dim_field;
      Field** fact_field;
      warp_join_info dim_info;

      // which table do we attach the join to?
      warp_pushdown_information* dim_table = NULL;
      warp_pushdown_information* fact_table = NULL;

      if(this_is_dim_table) {
        fact_table = f0_info;
        fact_field_mysql_name = f0->field_name;
        //fact_alias = f0->table_name;

        dim_table = f1_info;
        dim_field_mysql_name = f1->field_name;
        dim_alias = f1->table_name;
      } else {
        fact_table = f1_info;
        fact_field_mysql_name = f1->field_name;
        //fact_alias = f1->table_name;
        
        dim_table = f0_info;
        dim_field_mysql_name = f0->field_name;
        dim_alias = f0->table_name;
      }

      // find the field in the fact table
      for(fact_field = fact_table->fields; *fact_field; fact_field++) {
        //if((*fact_field)->field_name == fact_field_mysql_name) {
        //  break;
        //}
        if(strcasecmp((*fact_field)->field_name, fact_field_mysql_name) == 0) {
	  break;
        }
      }
      // have to find the field in the fact table or there was a serious error
      assert(*fact_field != NULL);

      // find the field in the dimension table
      for(dim_field = dim_table->fields; *dim_field; dim_field++) {
        //if((*dim_field)->field_name == dim_field_mysql_name) {    
        //   break;
        //}
        if(strcasecmp((*dim_field)->field_name, dim_field_mysql_name) == 0) {    
          break;
	}
      } 
      assert(*dim_field != NULL);

      dim_info.alias = dim_alias;
      dim_info.field = *dim_field;

      // attach the join to the fact table.  The actual pushdown will happen
      // when the table is first scanned (ie, ::rnd_init or ::index_init)
      fact_table->join_info.emplace(std::pair<Field*, warp_join_info>(*fact_field, dim_info));
      
      return 2;
    }
    
    /* NOT IN and NOT BETWEEN are left to MySQL */
    if((is_between || is_in) &&
       down_cast<Item_func_opt_neg *>(tmp)->negated) {
      return 0;
    }

    /* Strings compared with <, <=, >, >= and BETWEEN */
    if(is_between || op == " < " || op == " <= " || op == " > " ||
       op == " >= ") {
      std::string string_condition;
      if(warp_push_string_range(tmp, is_between, op, string_condition)) {
        where_clause += string_condition;
        return 1;
      }
    }

    /* Integer constants compared with a BIGINT column that can not be
       represented exactly as a double need FastBit's exact 64-bit integer
       syntax.  In an IN list or BETWEEN every value has to use it. */
    /* a TIMESTAMP column is stored in UTC, the temporal constants are times
       of the time zone of the session */
    bool timestamp_col = false;
    for (uint i = 0; i < tmp->arg_count; ++i) {
      if(arg[i]->type() == Item::Type::FIELD_ITEM &&
         warp_is_timestamp_type(
             down_cast<Item_field *>(arg[i])->field->real_type())) {
        timestamp_col = true;
      }
    }
    bool bigint_col = false;
    bool col_unsigned = false;
    bool needs_exact_syntax = false;
    for (uint i = 0; i < tmp->arg_count; ++i) {
      if(arg[i]->type() == Item::Type::FIELD_ITEM) {
        const Field *fld = down_cast<Item_field *>(arg[i])->field;
        if(fld->real_type() == MYSQL_TYPE_LONGLONG) {
          bigint_col = true;
          col_unsigned = fld->all_flags() & UNSIGNED_FLAG;
        }
      }
    }
    if(bigint_col) {
      for (uint i = 0; i < tmp->arg_count; ++i) {
        if(arg[i]->type() == Item::Type::INT_ITEM &&
           !warp_int_is_exact_double(arg[i])) {
          needs_exact_syntax = true;
        }
      }
      if(needs_exact_syntax && op == " LIKE ") {
        /* there is no 64-bit integer form of LIKE */
        return 0;
      }
    }

    /* BETWEEN AND IN() need some special syntax handling */
    for (uint i = 0; i < tmp->arg_count; ++i, ++arg) {
      if(i > 0) {
        if(!is_between && !is_in) { /* normal <, >, =, LIKE, etc */
          build_where_clause += op;
        } else {
          if(is_between) {
            if(i == 1) {
              build_where_clause += " BETWEEN ";
            } else {
              build_where_clause += " AND ";
            }
          } else {
            if(is_in) {
              if(i == 1) {
                build_where_clause += " IN (";
              } else {
                build_where_clause += ", ";
              }
            }
          }
        }
      }

      /* For most operators, only the column ordinal position is output here,
         but there is special handling for IS NULL and IS NOT NULL comparisons
         here too, because those functions only have one argument which is the
         field. These things only have meaning on NULLable columns of course,
         so there is special handling if the column is NOT NULL.
      */
      if((*arg)->type() == Item::Type::FIELD_ITEM) {
        auto field_index = ((Item_field *)(*arg))->field->field_index();
        field_may_be_null = ((Item_field *)(*arg))->field->is_nullable();

        /* this is the common case, where just the ordinal position is emitted
         */
        if(!is_is_null && !is_isnot_null) {
          /* If the field may be NULL it is necessary to check that that the
             NULL marker is zero because otherwise searching for 0 in a NULLable
             field would return true for NULL rows...
          */
          if(build_where_clause != "") {
            build_where_clause += " AND ";
          }
          if(field_may_be_null) {
            build_where_clause +=
                "(n" + std::to_string(field_index) + " = 0 AND ";
          }
          build_where_clause += "c" + std::to_string(field_index);
        } else {
          /* Handle IS NULL and IS NOT NULL, depending on NULLability */
          if(field_may_be_null) {
            if(is_is_null) {
              /* the NULL marker will be one if the value is NULL */
              build_where_clause += "(n" + std::to_string(field_index) + " = 1";
            } else if(is_isnot_null) {
              /* the NULL marker will be zero if the value is NOT NULL */
              build_where_clause += "(n" + std::to_string(field_index) + " = 0";
            }
          } else {
            if(is_is_null) {
              /* NOT NULL field is being compared with IS NULL so no rows can
               * match */
              build_where_clause += " 1=0 ";
            } else if(is_isnot_null) {
              /* NOT NULL field is being compared with IS NOT NULL so all rows
               * match */
              build_where_clause += " 1=1 ";
            }
          }
        }
        continue;
      }
      
      /* TODO:
         While there are some Fastbit functions that could be pushed down
         we don't handle that yet, but put this here as a reminder that it
         can be done at some point, as it will speed things up.  
         
         Special note: TEMPORAL values are passed down as an 
         Item_func::DATE_FUNC and the date is extracted from it.
      */
      if((*arg)->type() == Item::Type::CACHE_ITEM) {
        String str;
        //fixme: max_packet_length?
        str.reserve(1024*1024);
        (*arg)->print(current_thd, &str, QT_ORDINARY);
        if(
          memcmp("date",str.c_ptr()+9,4) == 0 || 
          strcasestr(str.c_ptr(), "interval ") != NULL ||
          strcasestr(str.c_ptr(), " as date") != NULL
          ) {
          uint64_t t;
          if(timestamp_col) {
            if(!warp_item_timestamp_value(*arg, current_thd, t)) return 0;
          } else {
            t=warp_item_temporal_value(*arg);
          }
          build_where_clause += std::to_string(t);
          continue;        
        }
        // only date_sub date_add etc are supported right now
        return 0;
      }   

      if((*arg)->type() == Item::Type::FUNC_ITEM) {
        auto func_item = dynamic_cast<Item_func *>(*arg);
        
        switch(func_item->functype()) {
          case Item_func::DATE_FUNC:
          case Item_func::ADDTIME_FUNC:
            {  
            uint64_t t;
            if(timestamp_col) {
              if(!warp_item_timestamp_value(*arg, current_thd, t)) return 0;
            } else {
              t=warp_item_temporal_value(*arg);
            }
            build_where_clause += std::to_string(t);
            }
            continue;
          break;
          default:
            return 0;
        }
      }
      
      if((*arg)->type() == Item::Type::INT_ITEM) {
        std::string val;
        if(!warp_format_int(*arg, needs_exact_syntax, col_unsigned, val)) {
          return 0;
        }
        build_where_clause += val;
        continue;
      }

      if((*arg)->type() == Item::Type::NULL_ITEM) {
        /* x <=> NULL is true for the rows where x is NULL */
        if(tmp->functype() == Item_func::Functype::EQUAL_FUNC) {
          return 0;
        }
        build_where_clause += " NULL ";
        continue;
      }

      // can't push down decimal comparisons as they are stored
      // as strings
      if((*arg)->type() == Item::Type::DECIMAL_ITEM) {
        //where_clause += "1=1";
        return 0;
      }

      if((*arg)->type() == Item::Type::REAL_ITEM) {
        String s;
        String *val = (*arg)->val_str(&s);
        build_where_clause += std::string(val->c_ptr());
        continue;
      }

      if((*arg)->type() == Item::Type::STRING_ITEM ||
         (*arg)->type() == Item::Type::HEX_BIN_ITEM) 
      {
        if(!is_eq) {
          return 0;
        }
        String s;
        String *val = (*arg)->val_str(&s);
        std::string escaped;
        char *ptr = val->c_ptr();
        for (unsigned int i = 0; i < val->length(); ++i) {
          char c = *(ptr + i);
          if(c == '\'') {
            escaped += '\\';
            escaped += '\'';
          } else if(c == 0) {
            escaped += '\\';
            escaped += '0';
          } else if(c == '\\') {
            escaped += "\\\\";
          } else {
            escaped += c;
          }
        }
        build_where_clause += "'" + escaped + "'";
        continue;
      }

      /* Any other kind of argument (for example a subquery) can not be
         translated into a FastBit condition, so MySQL has to evaluate the
         condition.  Every supported argument type is handled above and
         continues the loop. */
      return 0;
    }

    if(is_in) {
      build_where_clause += ')';
    }

    if(field_may_be_null) {
      build_where_clause += ')';
    }
  }
  
  where_clause += build_where_clause;
  
  // clause was pushed down successfully 
  return 1;
}

int ha_warp::bitmap_merge_join() {
  if(bitmap_merge_join_executed != false) {
    return 0;
  }
  bitmap_merge_join_executed = true;
  auto fact_pushdown_info=get_pushdown_info(table->in_use, table->alias);
  
  if(fact_pushdown_info == NULL) {
    return 0;
  }
  
  std::string dim_pushdown_clause = "";
  
  for(auto join_it = fact_pushdown_info->join_info.begin(); join_it != fact_pushdown_info->join_info.end(); ++join_it) {
    
    Field* fact_field = join_it->first;
    // don't try to push down blob or JSON columns for joins 
    if(fact_field->real_type() == MYSQL_TYPE_TINY_BLOB ||
       fact_field->real_type() == MYSQL_TYPE_MEDIUM_BLOB ||
       fact_field->real_type() ==  MYSQL_TYPE_BLOB ||
       fact_field->real_type() ==  MYSQL_TYPE_LONG_BLOB ||
       fact_field->real_type() ==  MYSQL_TYPE_JSON) {
      continue;
    }
    
    Field* dim_field = join_it->second.field;
    auto dim_pushdown_info = get_pushdown_info(table->in_use, join_it->second.alias);
    if(dim_pushdown_info == NULL) {
      continue;
    }
    
    std::string fact_colname = std::string("c") + std::to_string(fact_field->field_index());
    std::string fact_nullname = std::string("n") + std::to_string(fact_field->field_index());
    std::string dim_colname = std::string("c") + std::to_string(dim_field->field_index());
    std::string dim_nullname = std::string("n") + std::to_string(dim_field->field_index());
    std::string dim_alias = join_it->second.alias;

    //FIXME: this is going to be needed to properly support outer joins
    //bool fact_is_nullable = fact_field->is_nullable();
    bool dim_is_nullable = dim_field->is_nullable();

    if(dim_pushdown_info->filter == "") {
	    continue;
	    //dim_pushdown_info->filter="1=1";
    } 
    
    dim_pushdown_clause = dim_pushdown_info->filter;
    if(dim_is_nullable) {
      dim_pushdown_clause += " AND " + dim_nullname + "=0";
    } 
    /* open the dimension table to read the data - the pointers are stored on the pushdown
        info structure so that they can be re-used in the scan
    */
    warp_table_read_lock dim_data_lock(dim_pushdown_info->datadir);
    dim_pushdown_info->base_table = ibis::mensa::create(dim_pushdown_info->datadir);
    if(dim_pushdown_info->base_table == NULL) {
      continue;
    }
    
    dim_pushdown_info->filtered_table = 
    dim_pushdown_info->base_table->select(dim_pushdown_info->column_set.c_str(), dim_pushdown_clause.c_str());
    
    if(dim_pushdown_info->filtered_table == NULL) {
      continue;
    }

    auto dim_cursor = dim_pushdown_info->filtered_table->createCursor();      
    if(dim_cursor == NULL) {
      continue;
    }
  
    switch(dim_field->real_type()) {
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_GEOMETRY:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_JSON:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_TYPED_ARRAY:
        continue;
        break;
  
    }
    
    auto matches = new std::unordered_map<uint64_t, uint64_t>;
    uint64_t rownum = 0;
    while(dim_cursor->fetch() == 0) {
      
      ++rownum;   
  
      bool is_unsigned = fact_field->is_unsigned();
      int rc=0;
      switch(dim_field->real_type()) {

        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_YEAR: {
          if(is_unsigned) {
            unsigned int tmp = 0;
            rc = dim_cursor->getColumnAsUInt(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));
          } else {
            int tmp = 0;
            rc = dim_cursor->getColumnAsInt(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));            
          }
          
        } break;

        case MYSQL_TYPE_SHORT: {
          if(is_unsigned) {
            uint16_t tmp = 0;
            rc = dim_cursor->getColumnAsUShort(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));

          } else {
            int16_t tmp = 0;
            rc = dim_cursor->getColumnAsShort(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));
          }
        } break;

        case MYSQL_TYPE_LONG: {
          if(is_unsigned) {
            uint32_t tmp = 0;
            rc = dim_cursor->getColumnAsUInt(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));

          } else {
            int32_t tmp = 0;
            rc = dim_cursor->getColumnAsInt(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));

          }
        } break;

        case MYSQL_TYPE_LONGLONG: {
          uint64_t tmp = 0;
          if(is_unsigned) {
            rc = dim_cursor->getColumnAsULong(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));

          } else {
            int64_t tmp = 0;
            rc = dim_cursor->getColumnAsLong(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));

          }
        } break;

        case MYSQL_TYPE_INT24: {
          if(is_unsigned) {
            uint32_t tmp;
            rc = dim_cursor->getColumnAsUInt(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));

          } else {
            int32_t tmp;
            rc = dim_cursor->getColumnAsInt(dim_colname.c_str(), tmp);
            matches->insert(std::make_pair(tmp, rownum));

          }
        } break;

        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
        case MYSQL_TYPE_DATETIME2: {
          uint64_t tmp;
          rc = dim_cursor->getColumnAsULong(dim_colname.c_str(), tmp); 
          matches->insert(std::make_pair(tmp, rownum));

        } break;

        /* this should never happen but is here to avoid a warning */  
        default:
          continue;
      }
      if(rc != 0) {
        delete dim_cursor;
        delete matches;
        return -1;
      }
    } // end of fetch loop
    delete dim_cursor;
    if( matches->size() > 0 ) {
      auto filter_info = new warp_filter_info(fact_colname, dim_alias, dim_colname);
      fact_table_filters.insert(std::make_pair(filter_info, matches));
    } else {
      delete matches;
    }
    dim_pushdown_info->fact_table_filters = &fact_table_filters;

  } // end of dim tables loop  
  
  return 0;
}

//FIXME: maybe this is supposed to do something?
//everything seems to work with it just returning zero
//but some unexpected bugs around transactions might be
//lurking.  Need to double check storage engine 
//interface documentation.  Note: Those docs are outdated and
//I think this function is no longer used...
//external_lock seems to handle it?
int ha_warp::start_stmt(THD *, thr_lock_type) {
  return 0;
}

int ha_warp::register_trx_with_mysql(THD* thd, warp_trx* trx) {
  long long all_trx = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN | OPTION_TABLE_LOCK);
  if(all_trx && !trx->registered) {
    trx->registered = true;
    trans_register_ha(thd, true, warp_hton, const_cast<ulonglong*>(&(trx->trx_id)));
  }
  trans_register_ha(thd, false, warp_hton, const_cast<ulonglong*>(&(trx->trx_id)));
  return 0;
}

int ha_warp::external_lock(THD *thd, int lock_type){ 
    
  if (lock_type != F_UNLCK)  {
    auto current_trx = warp_get_trx(warp_hton, table->in_use);
    if(current_trx == NULL) current_trx = create_trx(table->in_use);
    assert(current_trx != NULL);
  
    register_trx_with_mysql(thd, current_trx);
    current_trx->lock_count++;

    if(lock_type == F_WRLCK) {
      current_trx->for_update = true;
    } else {
      current_trx->for_update = false;
    }

    /* serializable isolation level takes shared locks on all visible rows traveresed
      and so does LOCK IN SHARE MODE
    */
    if (current_trx->isolation_level == ISO_SERIALIZABLE) {
      current_trx->lock_in_share_mode=true;
    }

    enum_sql_command sql_command = (enum_sql_command)thd_sql_command(thd);
    /* Row events applied by a replica or by a BINLOG statement have no data
       modification command, but they do take a write lock. */
    const bool applying_row_events =
      lock_type == F_WRLCK &&
      (thd->slave_thread || sql_command == SQLCOM_BINLOG_BASE64_EVENT);
    if(applying_row_events ||
      sql_command == SQLCOM_UPDATE || sql_command == SQLCOM_UPDATE_MULTI ||
      sql_command == SQLCOM_INSERT ||
      sql_command == SQLCOM_REPLACE ||
      sql_command == SQLCOM_DELETE || sql_command == SQLCOM_DELETE_MULTI ||
      sql_command == SQLCOM_INSERT_SELECT ||
      sql_command == SQLCOM_LOAD ||
      sql_command == SQLCOM_ALTER_TABLE ||
      sql_command == SQLCOM_CREATE_TABLE)  {  
      // the first time a data modification statement is encountered
      // the transaction is marked dirty.  Registering the open
      // transaction prevents a transaction from seeing inserts
      // that are not visible to it and to still find duplicate
      // keys in transactions doing concurrent inserts
      if(!current_trx->dirty) {
        warp_state->register_open_trx(current_trx->trx_id);
        current_trx->open_registered = true;
        current_trx->dirty = true;
      }
    } 
  } else {
    // unlock the table
    cleanup_pushdown_info();
  }

  return 0;
}

warp_trx* ha_warp::create_trx(THD* thd) {
  trx_mutex.lock();
  auto trx = new warp_trx;
  trx->isolation_level = thd_get_trx_isolation(thd);
  thd->get_ha_data(warp_hton->slot)->ha_ptr = (void*)trx;
  trx->begin();
  warp_state->register_active_trx(trx->trx_id);
  trx->active_registered = true;
  trx->open_log();
  trx->autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN | OPTION_TABLE_LOCK);
  trx_mutex.unlock();
  return trx;
}

void warp_trx::open_log() {
  if(log == NULL) {
    log_filename = std::to_string(trx_id) + std::string(".txlog");
    log=fopen(log_filename.c_str(), "w+");
    if(log == NULL) {
      sql_print_error("Could not open transaction log %s", log_filename.c_str());
      assert(false);
    }
  }
}

/* a transaction object that goes away without commit or rollback (a failed
   autocommit statement, for example) must not stay on the list of open
   transactions, others would wait for it */
warp_trx::~warp_trx() {
  close_log();
  if(open_registered && warp_state != NULL) {
    warp_state->unregister_open_trx(trx_id);
  }
  if(active_registered && warp_state != NULL) {
    warp_state->unregister_active_trx(trx_id);
  }
}

void warp_global_data::unregister_open_trx(uint64_t trx_id) {
  commit_mtx.lock();
  open_trx.erase(trx_id);
  commit_mtx.unlock();
}

void warp_trx::close_log() {
  if(log != NULL) {
    fclose(log);
    log = NULL;
  }
  if(!log_filename.empty()) {
    if(unlink(log_filename.c_str()) != 0 && errno != ENOENT) {
      sql_print_error("Could not remove transaction log %s", log_filename.c_str());
    }
    log_filename.clear();
  }
}

void warp_trx::write_insert_log_rowid(uint64_t rowid) {
  int sz = 0;
  sz = fwrite(&insert_marker, sizeof(insert_marker), 1, log);
  if(sz == 0 || ferror(log) != 0) {
    sql_print_error("failed to write rowid into insert log: %s", log_filename.c_str());
    assert(false);
  }
  sz = fwrite(&rowid, sizeof(uint64_t), 1, log);
  if(sz == 0 || ferror(log) != 0) {
    sql_print_error("failed to write rowid into insert log: %s", log_filename.c_str());
    assert(false);
  }
}

void warp_trx::write_delete_log_rowid(uint64_t rowid) {
  int sz = 0;
  //sql_print_warning("Writing delete rowid %d", rowid);
  sz = fwrite(&delete_marker, sizeof(delete_marker), 1, log);
  if(sz == 0 || ferror(log) != 0) {
    sql_print_error("failed to write rowid into insert log: %s", log_filename.c_str());
    assert(false);
  }
  sz = fwrite(&rowid, sizeof(uint64_t), 1, log);
  if(sz == 0 || ferror(log) != 0) {
    sql_print_error("failed to write rowid into insert log: %s", log_filename.c_str());
    assert(false);
  }
}


int warp_trx::begin() {
  int retval = 0;
  if(trx_id == 0) {
    trx_id = warp_state->get_next_trx_id();
  } else {
    retval = 1;
  }
  return retval;
}

/* Removes the history locks that nobody needs any more.

   A history lock is made when a transaction T updates or deletes a row.  The
   transactions that are older than T must still see the row, and a
   transaction that is newer than T finds the delete in the delete bitmap once
   T committed.  So the lock of T can go when T is not open (it committed or
   rolled back) and no transaction that exists is older than T.  The
   transactions that exist include the read only ones, which are not open
   transactions (they do not write), see active_trx.  Without history locks a
   scan does not have to look for one for every row. */
void warp_global_data::cleanup_history_locks() {
  if(history_lock_count.load(std::memory_order_acquire) == 0) {
    return;
  }
  const uint64_t oldest_active = oldest_active_trx();

  commit_mtx.lock();
  history_lock_mtx.lock();

  /* nothing new since the last cleanup: no lock was added and the oldest
     transaction is the same one, so the same locks are needed */
  if(history_lock_inserts != history_cleaned_inserts ||
     oldest_active != history_cleaned_oldest) {
    auto history_lock_it = history_locks.begin();
    while(history_lock_it != history_locks.end()) {
      const uint64_t lock_trx_id = history_lock_it->second;
      if(lock_trx_id < oldest_active &&
         open_trx.find(lock_trx_id) == open_trx.end()) {
        dbug("Removing HISTORY lock of trx: " << lock_trx_id << " for rowid: " << history_lock_it->first);
        history_lock_it = history_locks.erase(history_lock_it);
      } else {
        ++history_lock_it;
      }
    }
    history_cleaned_inserts = history_lock_inserts;
    history_cleaned_oldest = oldest_active;
    history_lock_count.store(history_locks.size(), std::memory_order_release);
  }
  
  history_lock_mtx.unlock();
  commit_mtx.unlock();
}

// used when a transaction commits
// not called when statements commit
void warp_trx::commit() {
  commit_mtx.lock();
  int sz = 0;
  uint64_t rowid = 0;
  char marker;
  
  if(dirty) {
  
    if(warp_state->open_trx.find(trx_id) == warp_state->open_trx.end()) {
      sql_print_error("Open transaction is not registered as open");
      assert(false);
    }

    sz = fwrite(&commit_marker, sizeof(commit_marker), 1, log);
    if(sz != 1) {
      sql_print_error("failed to write commit marker into transaction log");
    }
    fflush(log);
    fsync(fileno(log));
    fseek(log, 0, SEEK_SET);
    while( (sz = fread(&marker, sizeof(marker), 1, log) == 1) ) {
      switch(marker) {
        case savepoint_marker:
          continue;
          break;

         case commit_marker:
          continue;
          break;
      
        case insert_marker:
          // insertions are already written to disk
          fseek(log, sizeof(uint64_t), SEEK_CUR);
          break;

        case delete_marker:
          fread(&rowid, sizeof(uint64_t), 1, log);
          if(sz != 1) {
            sql_print_error("transaction log read failed");
            assert(false);
          }
          
          warp_state->delete_bitmap->set_bit(rowid);
          continue;
          break;

        default:
          sql_print_error("transaction log read failed");
          assert(false);
          break;
      }
    } 

    // commit the deletes
    if(warp_state->delete_bitmap->commit() != 0) {
      sql_print_error("Failed to commit delete bitmap %s", warp_state->delete_bitmap->get_fname().c_str());
      assert(false);
    }
    
    // mark the transaction committed: set its bit in the commit bitmap and
    // make sure it is on disk
    if(warp_state->commit_bitmap->set_bit_direct(trx_id) != 0) {
      sql_print_error("Failed to write to the commit bitmap");
      assert(false);
    }

    // the transaction is not open any more
    warp_state->open_trx.erase(trx_id);
    open_registered = false;
  }
  close_log();
  
  commit_mtx.unlock();
}

// used when a transaction or statement rolls back
void warp_trx::rollback(bool all) {
  commit_mtx.lock();
  size_t savepoint_at = 0;
  uint64_t rowid = 0;
  char marker;
  if(dirty) {
    if(warp_state->open_trx.find(trx_id) == warp_state->open_trx.end()) {
      sql_print_error("Open transaction is not registered as open");
      assert(false);
    }
    int sz;
    if(all != ROLLBACK_STATEMENT) {
      sz = fwrite(&rollback_marker, sizeof(rollback_marker), 1, log);
      if(sz != 1) {
        sql_print_error("failed to write rollback marker into transaction log");
      }
    } 
    fflush(log);
    fsync(fileno(log));
    fseek(log, 0, SEEK_SET);
    clearerr(log);
  
    while((sz = fread(&marker, sizeof(marker), 1, log))) {
      switch(marker) {
        case rollback_marker:
          // nothing to do - end of log
        break;

        case savepoint_marker:
          savepoint_at = ftell(log) - sizeof(marker);
          continue;
        break;

        case insert_marker:
          if(all == ROLLBACK_STATEMENT && savepoint_at == 0) {
            fseek(log, sizeof(uint64_t), SEEK_CUR);
          }
          if(all == ROLLBACK_STATEMENT) {
            sz = fread(&rowid, sizeof(uint64_t), 1, log);
            if(feof(log)) {
              break;
            }
            if(sz != 1) {
              sql_print_error("could not read from transaction log");
              assert(false);
            }
            // delete this insert, which is equivalent to rolling it back
            if(warp_state->delete_bitmap->set_bit(rowid) != 0) {
              sql_print_error("could not set bit in deleted bitmap");
              assert(false);
            }
          }
          // do not have to roll back insertions as they will not be
          // in the commit bitmap
        break;

        case delete_marker:
          //row will be unlocked at trx delete
          //seek past the rowid
          fseek(log, sizeof(uint64_t), SEEK_CUR);
        break;
      }
    } 
    /* was dirty */
  }

  // need to commit the rolled back inserts to the delete bitmap
  if(all==ROLLBACK_STATEMENT) {
    if(warp_state->delete_bitmap->is_dirty()) {
      if(warp_state->delete_bitmap->commit() != 0) {
        sql_print_error("could not commit delete bitmap for rollback of statement");
        assert(false);
      }
    }
  
    // remove the savepoint data
    if(savepoint_at > 0) {
      fflush(log);
      ftruncate(fileno(log), savepoint_at);
      fsync(fileno(log));
    }
  } else {
    /* a rolled back transaction is not open any more and its bit in the
       commit bitmap is never set, so its rows are not visible */
    warp_state->open_trx.erase(trx_id);
    open_registered = false;
    close_log();
  } 

  commit_mtx.unlock();
}


warp_trx* warp_get_trx(handlerton* hton, THD* thd) {
  return (warp_trx*)thd->get_ha_data(hton->slot)->ha_ptr;
}

/* Commits a transaction to the WARP storage engine.  
   If the statement is an AUTOCOMMIT statement, then the 
   transaction is immediately committed.  If this is a
   multi-statement transaction, then the commit only
   happens when the commit_trx flag is true
   
   Only transactions that modified data need to be written
   to the commit log.  Read-only transaction don't need
   to do this work.
*/
int warp_commit(handlerton* hton, THD *thd, bool commit_trx) {
  auto current_trx = warp_get_trx(hton, thd);
  
  if(commit_trx || current_trx->autocommit) {
    if(current_trx->dirty) {
      current_trx->commit();
    }
  } else {
    /* this transaction is not ready to be committed to the
       storage engine because it is part of a multi-statement
       transaction
    */
    return 0;
  }
    
  /* if the transaction (autocommit or multi-statement) was 
     commited to disk, then the transaction information for
     the connection must be destroyed.
  */
  thd->get_ha_data(hton->slot)->ha_ptr = NULL;
  warp_state->free_locks(current_trx);
  /* the transaction does not exist any more when the locks are cleaned up,
     otherwise it would keep its own history locks */
  delete current_trx;
  warp_state->cleanup_history_locks();
  return 0;
}

/*  rollback a transaction in the WARP storage engine
    -------------------------------------------------------------
    rollback_trx will be false if either the transaction is
    autocommit or if this is a single statement in a 
    multi-statement transaction.  If it is a single statement
    in multi-statement transaction, then only the changes in that 
    statement are rolled back.
*/
int warp_rollback(handlerton* hton, THD *thd, bool rollback_trx) {
  warp_trx* current_trx = warp_get_trx(hton, thd);
  //if a statement failed, we need to rollback the insertions
  //
  if(rollback_trx) {
    if(current_trx->dirty) {
     // undo the changes
     current_trx->rollback(true);
    }
  } else {
    //statement rollback
    if(current_trx->dirty) {  
        current_trx->rollback(ROLLBACK_STATEMENT);
    }
    if(current_trx->autocommit) {
      //warp_state->mark_transaction_closed(trx->trx_id);
      current_trx->dirty = false;
    } else {
      return 0;
    }
  }
  
  // destroy the transaction
  warp_state->free_locks(current_trx);
  delete current_trx;
  thd->get_ha_data(hton->slot)->ha_ptr = NULL;
  warp_state->cleanup_history_locks();
  return 0;
}

bool ha_warp::is_row_visible_to_read(uint64_t rowid) {

  /* No row has a history lock and nothing was ever deleted (the usual case):
     every row is visible, there is nothing to look up */
  if(!warp_state->has_history_locks() &&
     !warp_state->delete_bitmap->may_have_bits()) {
    return true;
  }

  uint64_t history_trx_id = warp_state->get_history_lock(rowid);
  
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  
  if(history_trx_id == 0 
    || history_trx_id < current_trx->trx_id 
    || (history_trx_id > current_trx->trx_id && (current_trx->isolation_level != ISO_REPEATABLE_READ && current_trx->isolation_level != ISO_SERIALIZABLE))) {
    // no history lock or may have been commited into delete map
    // in a visible trx so have to check to see if the row is deleted
    if(is_deleted(current_rowid)) {
      return false;
    }
  } else {
    /* another transaction has deleted or updated this row */
    if(history_trx_id != current_trx->trx_id) {
      return true;
    }
    return false;
  }
  return true;
}

// checks the transaction marker to see if if this
// row is visible
bool ha_warp::is_trx_visible_to_read(uint64_t row_trx_id) {
  if(last_trx_id == row_trx_id) {
    return is_trx_visible;
  }
  last_trx_id = row_trx_id;

  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  
  //dbug("trx_id:" << current_trx->trx_id << " row_trx_id: " << row_trx_id);

  /* row belongs to current trx so it is visible */
  if(current_trx->trx_id == row_trx_id) {
    is_trx_visible = true;
    return is_trx_visible;
  }
  
  /* Only the rows of committed transactions are visible to others.  If the
     bit of the transaction is not set it is still open, was rolled back or
     could not be recovered (transaction ids start at 1). */
  if(row_trx_id == 0 || !warp_state->is_trx_committed(row_trx_id)) {
    is_trx_visible = false;
    return is_trx_visible;
  }

  /* older trx are visible if committed */
  if(row_trx_id < current_trx->trx_id) {
    is_trx_visible = true;
    return is_trx_visible;
  }

  // row_trx_id is newer and RR or SERIALIZABLE thus not visible due to isolation level
  if (current_trx->isolation_level == ISO_REPEATABLE_READ || current_trx->isolation_level == ISO_SERIALIZABLE) { 
    is_trx_visible = false;
    return is_trx_visible;
  }

  // if RC or RU and the trx is committed it is visible
  is_trx_visible = true;
  return is_trx_visible;

}

/* Internal functions for maintaining and working with
   WARP tables
*/
int warp_upgrade_tables(uint16_t version) {
  if(version == 0) {
    ibis::partList parts;
    if(ibis::util::gatherParts(parts, ".") == 0) {
      // No tables so nothing to do!
      return 0;
    }

    for (auto it = parts.begin(); it < parts.end(); ++it) {
      auto part = *it;
      bool found_trx_column = false;
      auto colnames = part->columnNames();
      for(auto colit = colnames.begin(); colit < colnames.end(); ++colit) {
        if(std::string(*colit) == "t") {
          found_trx_column = true;
          break;
        }
      }
      if(!found_trx_column) {
        ibis::tablex* writer = ibis::tablex::create();
        const char* datadir = part->currentDataDir();
        const std::string metafile = std::string(datadir) + "/-part.txt";
        const std::string backup_metafile = std::string(datadir) + "/-part.txt.old";
        writer->readNamesAndTypes(metafile.c_str());
        writer->addColumn("t", ibis::ULONG, "transaction identifier");
        if(rename(metafile.c_str(), backup_metafile.c_str()) != 0) {
          sql_print_error("metadata rename failed %s -> %s", metafile.c_str(), backup_metafile.c_str());
          assert(false);
        }
        if(writer->writeMetaData(datadir) == (int)(part->columnNames().size() + 1)) {
          if(unlink(backup_metafile.c_str()) != 0) {
            sql_print_error("metadata write failed %s -> %s", metafile.c_str());
            assert(false);
          }
        } else {
          std::string logmsg = "Metadata write failed for metadata file %s";
          sql_print_error(logmsg.c_str(), metafile);
          assert(false);
        }
        std::string logmsg = "Upgraded WARP partition %s to include transaction identifiers";
        sql_print_error(logmsg.c_str(), datadir);
        writer->clearData();
        delete writer;
        std::string column_fname = std::string(datadir) + "/t";
        if(part->nRows() > 0) {
          /* one zero transaction id (uint64_t) per existing row */
          std::vector<uint64_t> zeros(part->nRows(), 0);
          if(ibis::zfile::writeWhole(column_fname.c_str(), zeros.data(),
                                     zeros.size() * sizeof(uint64_t),
                                     sizeof(uint64_t),
                                     ibis::zfile::level() > 0) != 0) {
            sql_print_error("Failed to zerofill file %s", column_fname.c_str());
            assert(false);
          }
        }
      }
    }
  } else {
    sql_print_error( 
      "On disk WARP version is greater than storage engine version. Engine version: %d but on disk version is %d", 
      WARP_VERSION,
      version
    );
    assert(false);
  }
  return 0;
}

/* The name of the log of a transaction is its transaction id and ".txlog" in
   the data directory, for example 1184.txlog.  The logs of the delete bitmap
   (deletes.warp.txlog) and the savepoint logs have other names. */
static bool is_trx_log_name(const char *name) {
  const char *ptr = name;
  if(*ptr < '0' || *ptr > '9') {
    return false;
  }
  while(*ptr >= '0' && *ptr <= '9') {
    ++ptr;
  }
  return strcmp(ptr, ".txlog") == 0;
}

/* Removes the transaction logs from the data directory, which is the current
   directory of the server. */
static void remove_stale_trx_logs() {
  DIR *dir = opendir(".");
  if(dir == NULL) {
    sql_print_error("Could not open the data directory to remove transaction logs");
    return;
  }
  struct dirent *ent;
  while((ent = readdir(dir)) != NULL) {
    if(!is_trx_log_name(ent->d_name)) {
      continue;
    }
    if(unlink(ent->d_name) != 0 && errno != ENOENT) {
      sql_print_error("Could not remove transaction log %s", ent->d_name);
    }
  }
  closedir(dir);
}

warp_global_data::warp_global_data() {
  uint64_t on_disk_version = 0;
  bool shutdown_ok = false;
  assert(check_state() == true);

  fp = fopen(warp_state_file.c_str(), "rb+");
  //if fp == NULL then the database is being initialized for the first time
  if(fp == NULL) {
    sql_print_error("First time startup - initializing new WARP database.");
    next_rowid = 1;
    next_trx_id = 1;
    fp = fopen(warp_state_file.c_str(), "w+"); 
    if(fp == NULL) {
      sql_print_error("Could not open for writing: %s", warp_state_file.c_str());
      assert(false);
    }
    write();
    on_disk_version = WARP_VERSION;
    shutdown_ok = true;
  } else {
    on_disk_version = get_state_and_return_version();
    shutdown_ok = was_shutdown_clean();
  }  
  
  /* No transaction is active when the server starts, so every transaction
     log is a leftover of a crash (or of a transaction that did not remove its
     log).  There is no need to roll back the insertions the logs describe, the
     transactions associated with them will not be in the commit list and any
     deletions associated with those transactions will be rolled back
     automatically when the bitmaps are opened (the logs of the bitmaps have
     other names and are used for that). */
  remove_stale_trx_logs();

  if(!shutdown_ok) {
    if(!repair_tables()) {
      assert("Table repair failed. Database could not be initialized");
    }
  } 
  
  // this file will be rewritten at clean shutdown
  unlink(shutdown_clean_file.c_str());
 
  /* The committed transactions are a bitmap, a bit for every transaction
     id, which is read through a memory map.  Nothing has to be loaded here.
     The file is created if it does not exist. */
  try {
    commit_bitmap = new sparsebitmap(commit_bitmap_file, LOCK_SH);
  } catch(...) {
    sql_print_error("Could not open commit bitmap: %s", commit_bitmap_file.c_str());
    assert(false);
  }

  /* older versions kept a list of the committed transaction ids */
  if(!migrate_commit_list()) {
    sql_print_error("Could not convert %s to the commit bitmap %s",
                    commit_filename.c_str(), commit_bitmap_file.c_str());
    assert(false);
  }
  
  // this will create the deletes.warp bitmap if it does not exist
  try {
     delete_bitmap = new sparsebitmap(delete_bitmap_file, LOCK_SH); 
  } catch(...) {
    sql_print_error("Could not open delete bitmap: %s", delete_bitmap_file.c_str());
    assert(false);
  }
  
  // if tables are an older version on disk, proceed with upgrade process
  if(on_disk_version != WARP_VERSION) {
     if(!warp_upgrade_tables(on_disk_version)) {
       sql_print_error("WARP upgrade tables failed");
      assert(false);
    }
    // will write new version information to disk
    // asserts if writing fails
    write();
  }
  // ALL OK - DATABASE IS OPEN AND INITIALIZED!
}

/* The versions of WARP before the commit bitmap kept the committed
   transactions in the file commits.warp, a list of 8 byte transaction ids,
   which was loaded into memory when the server started.  The ids of the list
   are set in the commit bitmap and the file is renamed so that this is done
   once.  Returns false if the list could not be converted. */
bool warp_global_data::migrate_commit_list() {
  struct stat st;
  if(stat(commit_filename.c_str(), &st) != 0) {
    return true; /* nothing to convert */
  }
  FILE *old_list = fopen(commit_filename.c_str(), "rb");
  if(old_list == NULL) {
    return false;
  }
  uint64_t trx_id = 0;
  uint64_t converted = 0;
  while(fread(&trx_id, sizeof(trx_id), 1, old_list) == 1) {
    if(trx_id == 0) {
      continue;
    }
    if(commit_bitmap->set_bit_direct(trx_id, false) != 0) {
      fclose(old_list);
      return false;
    }
    ++converted;
  }
  fclose(old_list);
  if(commit_bitmap->sync_direct() != 0) {
    return false;
  }
  std::string converted_name = commit_filename + ".converted";
  if(rename(commit_filename.c_str(), converted_name.c_str()) != 0) {
    return false;
  }
  sql_print_information("WARP: converted %llu committed transactions from %s to the commit bitmap %s",
                        (unsigned long long)converted, commit_filename.c_str(),
                        commit_bitmap_file.c_str());
  return true;
}

/* Check the state of the database. 
  1) the state file must exist
  2) the state file must be the correct size
  3) the commit bitmap must exist on disk
  4) 
  If all of these things are not correct then print an error message
  and crash the database, unless the state file does not exist AND
  the commit bitmap do not exist, which means this is the first time
  that WARP is being initialized UNLESS WARP tables already exist.
  If no WARP tables exist, this is the first WARP initialization
  (which means mysqld --initialize is running) and the files are 
  created.
*/
bool warp_global_data::check_state() {
  struct stat st;
  int state_exists = (stat(warp_state_file.c_str(), &st) == 0);
  /* the committed transactions are in the commit bitmap, or in the list of
     older versions that is converted when the server starts */
  int commit_file_exists = (stat(commit_bitmap_file.c_str(), &st) == 0) ||
                           (stat(commit_filename.c_str(), &st) == 0);

  if((state_exists && !commit_file_exists)) {
    sql_print_error("warp_state found but the commit bitmap (%s) is missing! Database can not be initialized.",
                    commit_bitmap_file.c_str());
    return false;
  } 
  
  if((!state_exists && commit_file_exists)) {
    sql_print_error("the commit bitmap (%s) is found but warp_state is missing! Database can not be initialized.",
                    commit_bitmap_file.c_str());
    return false;
  } 
  
  auto parts = new ibis::partList;
  int has_warp_tables = ibis::util::gatherParts(*parts, std::string(".").c_str());
  if(!state_exists && !commit_file_exists && has_warp_tables > 0) {
    sql_print_error("WARP tables found but database state is missing! This may be a beta 1 database. WARP can not be initialized.");
    return false;
  }
  delete parts;
  return true;
}

uint64_t warp_global_data::get_next_trx_id() {
  mtx.lock();
  ++next_trx_id;
  write();
  mtx.unlock();
  return next_trx_id;
}

uint64_t warp_global_data::get_next_rowid_batch(uint64_t count) {
  mtx.lock();
  next_rowid += count;
  write();
  mtx.unlock();
  return next_rowid;
}

/* Only transactions that are for write are registered as open
   called in ::external_lock when a transaction first makes changes
*/
void warp_global_data::register_open_trx(uint64_t trx_id) {
  commit_mtx.lock();
  open_trx.insert(trx_id);
  commit_mtx.unlock();
}

/* A transaction is open if it made changes and has not committed or rolled
   back yet.  Committed transactions have their bit set in the commit bitmap.
*/
bool warp_global_data::is_transaction_open(uint64_t trx_id) {
  commit_mtx.lock();
  bool retval = (open_trx.find(trx_id) != open_trx.end());
  commit_mtx.unlock();
  return retval;
}

int warp_global_data::create_lock(uint64_t rowid, warp_trx* trx, int lock_type) {
  uint spin_count = 0;
  struct timespec sleep_time;
  struct timespec remaining_time;
  sleep_time.tv_sec = (time_t)0;
  sleep_time.tv_nsec = 100000000L; // sleep a millisecond
  
  // each sleep beyond the spin locks increments the 
  // waiting time
  ulonglong max_wait_time = THDVAR(current_thd, lock_wait_timeout) * 100000000L;
  ulonglong wait_time = 0;
  // create a new lock for our lock
  // will be deleted and replaced if 
  // we discover we already have this 
  // lock!  
  warp_lock new_lock;
    
  // history locks are taken after EX_LOCKS are granted
  // for more information about history locks, see 
  // ha_warp::update_row comments
  if(lock_type == LOCK_HISTORY) {
    add_history_lock(rowid, trx->trx_id);
    return LOCK_HISTORY;
  }

  new_lock.holder = trx->trx_id;
  new_lock.waiting_on = 0;
  new_lock.lock_type = lock_type;
retry_lock:
  lock_mtx.lock();
  // this is used to iterate over all the locks held
  // for this row.  
  auto it = row_locks.find(rowid);

  // check to see if any row locks exist for this
  // row
  if(it == row_locks.end()) {
    // row is not locked so lock can proceed without
    // checking anything further!
    row_locks.emplace(std::pair<uint64_t, warp_lock>(rowid, new_lock));
    lock_mtx.unlock();
    return lock_type;
  } else {
    // row is already locked.  Only the locks of this row are looked at.
    auto row_range = row_locks.equal_range(rowid);
    for(auto it2 = row_range.first; it2 != row_range.second; ++it2) {
      warp_lock test_lock = it2->second;

      // this lock will be released because of deadlock
      // so go to sleep and wait for it to be released
      // so that we don't possibly hit another deadlock
      // from the same trx before all the locks are 
      // released as the transaction closes
      if(test_lock.lock_type == LOCK_DEADLOCK) {
        lock_mtx.unlock();
        goto sleep;
      }

      // the current transaction already holds a lock on this row
      if(test_lock.holder != trx->trx_id) {
        
        if(test_lock.lock_type != LOCK_HISTORY && (test_lock.lock_type != LOCK_SH && lock_type != LOCK_SH)) {
          lock_mtx.unlock();
          goto sleep;
        }

      } else {
        if(test_lock.waiting_on != 0) {
          // does the waiting transaction still exist?
          if(is_transaction_open(test_lock.waiting_on)) {
            lock_mtx.unlock();
            goto sleep;
          }
          new_lock.waiting_on = 0;
          row_locks.erase(it2);
          row_locks.emplace(std::pair<uint64_t, warp_lock>(rowid, new_lock));
          lock_mtx.unlock();
          return lock_type;
        }

        // This transaction has a lock on the row.  A lock is never made
        // weaker: SH < WRITE_INTENTION (SELECT ... FOR UPDATE) < EX.
        // Asking for what is held already, or for less, keeps the lock
        // (a LOCK IN SHARE MODE read after a FOR UPDATE read, for example),
        // asking for more upgrades the lock that is held.  The lock is
        // changed in place: nothing is erased while the locks are walked.
        auto strength = [](int type) {
          switch(type) {
            case LOCK_SH: return 1;
            case WRITE_INTENTION: return 2;
            case LOCK_EX: return 3;
            default: return 0;
          }
        };
        if(strength(test_lock.lock_type) < strength(lock_type)) {
          it2->second.lock_type = lock_type;
        }
        lock_mtx.unlock();
        return lock_type;
      }
      
      // this lock is a shared lock by somebody else
      // and this lock request is for a shared lock
      // so keep searching - we will grant the lock request
      // as long as no conflicting EX_LOCK is found
      // AND as long as this lock is not waiting on another
      // transaction
      if(test_lock.lock_type == LOCK_SH && new_lock.lock_type == LOCK_SH) {
        // if the existing  shared lock is not waiting on an EX lock
        // the shared lock can be granted, othewrise
        // we have to wait on this lock
        if(test_lock.waiting_on == 0) {
          // iterate because this trx might already hold a shared lock 
          // to reuse
          continue;
        }
        // the shared lock is waiting on an EX lock!
        // can not acquire the shared lock right now
        // will sleep a bit if spinlocks are exhausted and 
        // will error out if lock_wait_timeout is exhausted
        new_lock.waiting_on = test_lock.waiting_on;
        row_locks.emplace(std::pair<uint64_t, warp_lock>(rowid, new_lock));
        lock_mtx.unlock();
        goto sleep;
      }
      
      // If new_lock points to an existing lock and the 
      // other transaction is already waiting on this
      // lock, then a DEADLOCK is detected!
      // this transaction will be rolled back
      if((lock_type == LOCK_EX || lock_type == WRITE_INTENTION) && test_lock.waiting_on == new_lock.holder) {
        new_lock.lock_type = LOCK_DEADLOCK;
        row_locks.emplace(std::pair<uint64_t, warp_lock>(rowid, new_lock));
        lock_mtx.unlock();
        return LOCK_DEADLOCK;
      } else {
        // have to wait to upgrade the lock
        lock_mtx.unlock();
        goto sleep;
      }
    }
  }  

  new_lock.waiting_on = 0;
  // insert the new lock
  row_locks.emplace(std::pair<uint64_t, warp_lock>(rowid, new_lock));
  
  lock_mtx.unlock();
  return lock_type;

sleep:
  //lock_mtx.unlock();
  //fixme - make this configurable
  if(spin_count++ > 0) {
    
    int err = nanosleep(&sleep_time, &remaining_time);
    if(err < 0) {
      if(err == EINTR) {
        sleep_time=remaining_time;
        goto sleep;
      }
      return ER_LOCK_ABORTED;
     } 
  }
  wait_time += sleep_time.tv_nsec;
  if(wait_time >= max_wait_time) {
    return ER_LOCK_WAIT_TIMEOUT;
  }

  // lock sleep completed 
  goto retry_lock;

  // never reached
  return -1;
  
}

/* When the database shuts down clean it writes the
   warp_clean_shutdown file to disk
*/
bool warp_global_data::was_shutdown_clean() {
  struct stat st;
  if(stat(shutdown_clean_file.c_str(), &st) != 0) {
    return false;
  }
  return st.st_size == sizeof(uint8_t);
}

uint64_t warp_global_data::get_state_and_return_version() {
  struct on_disk_state state_record1;
  struct on_disk_state state_record2;
  struct on_disk_state* state_record;
  
  fread(&state_record1, sizeof(struct on_disk_state), 1, fp);
  if(ferror(fp) != 0) {
      sql_print_error("Failed to read state record one from warp_state");
    return 0;
  }
  fread(&state_record2, sizeof(struct on_disk_state), 1, fp);
  if(ferror(fp) != 0) {
    sql_print_error("Failed to read state record two from warp_state");
    return 0;
  }
  
  if(state_record2.state_counter == 0 && state_record1.state_counter == 0) {
    sql_print_error("Both state records are invalid.");
    return 0;
  }
  
  if(state_record2.state_counter == 0) {
    state_record = &state_record1;
  }
  
  if(state_record2.state_counter > state_record1.state_counter) {
    state_record = &state_record2;
  } else {
    state_record = &state_record1;
  }

  next_trx_id = state_record->next_trx_id;
  next_rowid = state_record->next_rowid;
  state_counter = state_record->state_counter;

  return state_record->version;
}

bool warp_global_data::repair_tables() {
  return true;
}

void warp_global_data::write_clean_shutdown() {
  FILE *sd = NULL;
  sd = fopen(shutdown_clean_file.c_str(), "w");
  if(!sd) {
    sql_print_error("could not open shutdown file");
    assert(false);
  }
  uint8_t one = 1;
  fwrite(&one, sizeof(uint8_t), 1, sd);
  if(ferror(sd) != 0) {
    sql_print_error("could not write shutdown file");
  }
  fflush(sd);
  fsync(fileno(sd));
  fclose(sd);
}

// the data is written to disk twice because if the database 
// or system crashes during the write, the state information
// would be corrupted!  
void warp_global_data::write() {
  struct on_disk_state record;
  // write the second record
  memset(&record, 0, sizeof(struct on_disk_state));
  // write the second record first.  If this fails, then the
  // old record will be used when the database restarts.
  if(fseek(fp, sizeof(struct on_disk_state), SEEK_SET) != 0) {
    sql_print_error("seek on warp_state failed!");
    assert(false);
  }
  fwrite(&record, sizeof(struct on_disk_state), 1, fp);
  if(ferror(fp) != 0) {
    sql_print_error("Write to database state failed");
    assert(false);
  }
  fflush(fp);
  if(fseek(fp, sizeof(struct on_disk_state), SEEK_SET) != 0) {
    sql_print_error("seek on warp_state failed!");
    assert(false);
  }
  record.next_rowid  = next_rowid;
  record.next_trx_id = next_trx_id;
  record.version     = WARP_VERSION;
  record.state_counter = ++state_counter;
  fwrite(&record, sizeof(struct on_disk_state), 1, fp);
  if(ferror(fp) != 0) {
    sql_print_error("Write to database state failed");
    assert(false);
  }
  fflush(fp);
  if(fsync(fileno(fp)) != 0) {
    sql_print_error("fsync to database state failed");
    assert(false);
  }

  // write the first record
  rewind(fp);   
  memset(&record, 0, sizeof(struct on_disk_state));
  fwrite(&record, sizeof(struct on_disk_state), 1, fp);
  if(ferror(fp) != 0) {
    sql_print_error("Write to database state failed");
    assert(false);
  }  
  fflush(fp);
  record.next_rowid  = next_rowid;
  record.next_trx_id = next_trx_id;
  record.version     = WARP_VERSION;
  record.state_counter = ++state_counter;
  fwrite(&record, sizeof(struct on_disk_state), 1, fp);
  if(ferror(fp) != 0) {
    sql_print_error("Write to database state failed");
    assert(false);
  }  
  fflush(fp);
  if(fsync(fileno(fp)) != 0) {
    sql_print_error("fsync to database state failed");
    assert(false);
  }
}

// not currently used - here for completeness
int warp_global_data::unlock(uint64_t rowid, warp_trx* trx) {
  lock_mtx.lock();
  // row is not locked!
  for(auto it = row_locks.find(rowid); it != row_locks.end();++it) {
    if(it->second.holder == trx->trx_id) {
      row_locks.erase(it);
      break;
    }
  }
  lock_mtx.unlock();
  return 0;

}
// an EX_LOCK can be downgraded to a history lock
// this function is here for completeness but it
// is not currently used as ::update_row and 
// ::delete_row automatically take history locks
int warp_global_data::downgrade_to_history_lock(uint64_t rowid, warp_trx* trx) {
  // row is not locked!
  lock_mtx.lock();
  for(auto it = row_locks.find(rowid); it != row_locks.end();++it) {
    if(it->second.holder == trx->trx_id) {
      row_locks.erase(it);
      break;
    }
  }
  lock_mtx.unlock();
  // any trx open at or before this transaction will see the 
  // history lock - no need to check the delete bitmap for
  // any row that has a history lock - it was deleted 
  // and is no longer visible to newer transactions
  // if a history lock doesn't exist the deleted bitmap
  // will be checked
  add_history_lock(rowid, trx->trx_id);

  return 0;
}

int warp_global_data::free_locks(warp_trx* trx) {
  lock_mtx.lock();
  restart:
  for(auto it = row_locks.begin(); it != row_locks.end();++it) {
    if(it->second.holder == trx->trx_id) {
      row_locks.erase(it);
      // deleting the lock invalidates the iterator...
      // so restart at the beginning.   There is probably
      // a better way to do this...
      goto restart;
    }
  }
  lock_mtx.unlock();
  return 0;
}

// returns 0 if no history lock or the trx_id that created
// the lock otherwise
uint64_t warp_global_data::get_history_lock(uint64_t rowid) {
  /* no row has a history lock (the usual case) */
  if(history_lock_count.load(std::memory_order_acquire) == 0) {
    return 0;
  }
  std::shared_lock<std::shared_mutex> guard(history_lock_mtx);
  auto it = history_locks.find(rowid);
  if(it == history_locks.end()) {
    return 0;
  }
  return it->second;
}

void warp_global_data::add_history_lock(uint64_t rowid, uint64_t trx_id) {
  std::unique_lock<std::shared_mutex> guard(history_lock_mtx);
  history_locks.emplace(std::pair<uint64_t, uint64_t>(rowid, trx_id));
  ++history_lock_inserts;
  history_lock_count.store(history_locks.size(), std::memory_order_release);
}

void warp_global_data::register_active_trx(uint64_t trx_id) {
  std::lock_guard<std::mutex> guard(active_trx_mtx);
  active_trx.insert(trx_id);
}

void warp_global_data::unregister_active_trx(uint64_t trx_id) {
  std::lock_guard<std::mutex> guard(active_trx_mtx);
  active_trx.erase(trx_id);
}

uint64_t warp_global_data::oldest_active_trx() {
  std::lock_guard<std::mutex> guard(active_trx_mtx);
  return active_trx.empty() ? UINT64_MAX : *active_trx.begin();
}

uint64_t warp_global_data::active_trx_total() {
  std::lock_guard<std::mutex> guard(active_trx_mtx);
  return active_trx.size();
}

warp_global_data::~warp_global_data() {
  delete commit_bitmap;
  commit_bitmap = NULL;
  /*
  if(commit_bitmap->close(1) != 0) {
    sql_print_error("Could not close bitmap %s", commit_bitmap->get_fname().c_str());
    assert(false);
  }*/
  if(delete_bitmap->close(1) != 0) {
    sql_print_error("Could not close bitmap %s", delete_bitmap->get_fname().c_str());
    assert(false);
  }
  delete delete_bitmap;
  delete_bitmap=NULL;
  write();
  fclose(fp);
  
  write_clean_shutdown();
}

std::string ha_warp::explain_extra() const { 
  if (pushed_cond != nullptr) {
    return ", with pushed condition: " + ItemToString(pushed_cond);
  }
  return "";
}

// get the number of rows in all the tables in the current schema
std::unordered_map<const char*, uint64_t> get_table_counts_in_schema(char* table_dir) {
  /* the counts are kept for the next call, by all the connections */
  static std::mutex table_counts_mtx;
  std::lock_guard<std::mutex> table_counts_guard(table_counts_mtx);
  static std::unordered_map<const char*, uint64_t> table_counts;
  ibis::partList parts;
  if(!table_counts.empty()) return table_counts;

  char* schema_dir = strdup(table_dir);
  schema_dir = dirname(schema_dir);
  ibis::util::gatherParts(parts, schema_dir, true);
  for(auto part_it = parts.begin(); part_it < parts.end(); ++part_it) {
    ibis::part* part = *part_it;
    // the top-level partition ends in .data, the other partitions are ./data/pXXX
    if(strstr(part->currentDataDir(), ".data/") == NULL) {
      ibis::table* tbl = ibis::mensa::create(part->currentDataDir());
      if(!tbl) {
        table_counts.emplace(part->currentDataDir(), 0);
        continue;
      }
      table_counts.emplace(part->currentDataDir(), tbl->nRows());
      delete tbl;
    }
    
  }
  
  free(schema_dir);
  
  return table_counts;
}

// return the path to the table with the most rows in the database
const char* get_table_with_most_rows(std::unordered_map<const char*, uint64_t>* table_counts, std::unordered_map<std::string, bool> query_tables) {
  uint64_t max_cnt = 0;
  static const char* table_with_max_cnt = NULL;
  if(table_with_max_cnt != NULL) return table_with_max_cnt;

  for(auto it = table_counts->begin(); it != table_counts->end(); it++) {
    auto it2=query_tables.find(std::string(it->first));
    if(it2 == query_tables.end()) {
      continue;
    }
      
    if(it->second >= max_cnt) {
      max_cnt = it->second;
      table_with_max_cnt = it->first;
    }
  }
  return table_with_max_cnt;
}

// return the path to the table with the most rows in the database
uint64_t get_least_row_count(std::unordered_map<const char*, uint64_t>* table_counts) {
  static std::mutex least_row_count_mtx;
  std::lock_guard<std::mutex> least_row_count_guard(least_row_count_mtx);
  static uint64_t min_cnt = -1ULL;
  if(min_cnt < -1ULL) {
    return min_cnt;
  }
  
  for(auto it = table_counts->begin(); it != table_counts->end(); it++) {
    if(it->second <= min_cnt) {
      min_cnt = it->second;
    }
  }
  return min_cnt;
}

uint64_t get_pushdown_info_count(THD* thd) {

  pushdown_mtx.lock();
  if(pd_info.empty()) { 
    pushdown_mtx.unlock();
    return 0;
  }

  auto it = pd_info.find(thd);
  if(it == pd_info.end()) {
    pushdown_mtx.unlock();
    return 0;
  }
  
  auto pushdown_info_map = it->second;
  if(pushdown_info_map == NULL) {
    pushdown_mtx.unlock();
    return 0;
  }
  
  uint64_t retval = pushdown_info_map->size();

  pushdown_mtx.unlock();

  return retval;

}

warp_pushdown_information* get_pushdown_info(THD* thd, const char* alias) {
  pushdown_mtx.lock();
  // The pushdown information will be missing if the referenced table
  // belongs to a different storage engine
  if(pd_info.empty()) { 
    pushdown_mtx.unlock();
    return NULL;
  }

  auto it = pd_info.find(thd);
  if(it == pd_info.end()) {
    pushdown_mtx.unlock();
    return NULL;
  }

  // If it was found above it is a WARP table and the map 
  // information should be set - if it is NULL then something is broken!
  auto pushdown_info_map = it->second;
  if(pushdown_info_map == NULL) {
    pushdown_mtx.unlock();
    return NULL;
  }
  
  if(pushdown_info_map->empty()) { 
    pushdown_mtx.unlock();
    return NULL; 
  }
  
  int is_empty = true;
  for(auto it2 = pushdown_info_map->begin();it2!=pushdown_info_map->end(); ++it2) {
    if(it2->first == std::string(alias)) {
      is_empty = false;
      break;
    }
  }
  if(is_empty) {
    pushdown_mtx.unlock();
    return NULL;
  }
  auto it2 = pushdown_info_map->find(alias);
  if(it2 == pushdown_info_map->end()) {
    pushdown_mtx.unlock();
    return NULL;
  }
  /*  
  if(it2->first == NULL) {
    pushdown_mtx.unlock();
    return NULL;
  }*/
  
  pushdown_mtx.unlock();
  // return the pushdown info
  return it2->second;
}

warp_pushdown_information* get_or_create_pushdown_info(THD* thd, const char* alias, const char* data_dir_name) {
  std::unordered_map<std::string, warp_pushdown_information*> *pushdown_info_map;
  warp_pushdown_information* pushdown_info;

  pushdown_mtx.lock();
   
  auto it = pd_info.find(thd);
  if(it == pd_info.end()) {
    pushdown_info_map = new std::unordered_map<std::string, warp_pushdown_information*>;
    pushdown_info = new warp_pushdown_information();
    //map the alias used by MySQL to the directory of the Fastbit table
    pushdown_info->datadir = data_dir_name;
    pushdown_info_map->emplace(std::pair<std::string, warp_pushdown_information*>(std::string(alias), pushdown_info));
    pd_info.emplace(std::pair<THD*, std::unordered_map<std::string, warp_pushdown_information*>*>(thd, pushdown_info_map));
  } else {
    pushdown_info_map = it->second;
    auto it2 = pushdown_info_map->find(alias);
    if(it2 == pushdown_info_map->end()) {
      pushdown_info = new warp_pushdown_information();
      pushdown_info->datadir = data_dir_name;
      pushdown_info_map->emplace(std::pair<std::string, warp_pushdown_information*>(std::string(alias), pushdown_info));
    } else {
      pushdown_info = it2->second;
    }
  }
  
  pushdown_mtx.unlock();
  
  return pushdown_info;
}
