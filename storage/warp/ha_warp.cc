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
static void forget_partitions(const char *table_name);
static std::string warp_autoinc_file(const char *data_dir);
// Stuff for shares */
mysql_mutex_t warp_mutex;
static std::unique_ptr<collation_unordered_multimap<std::string, WARP_SHARE *>>
    warp_open_tables;

static handler *warp_create_handler(handlerton *hton, TABLE_SHARE *table,
                                    bool partitioned, MEM_ROOT *mem_root);

/* WARP takes no table locks, has no unique keys and no foreign keys: there
   is nothing to say about partitioning */
static uint warp_partition_flags() { return 0; }

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
static void warp_purge_start();
static void warp_purge_stop();

#include <condition_variable>
#include "warp_locks.h"

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

static int show_warp_purged_rows(MYSQL_THD, SHOW_VAR *var, char *buff);
static int show_warp_purged_partitions(MYSQL_THD, SHOW_VAR *var, char *buff);
static int show_warp_purge_failures(MYSQL_THD, SHOW_VAR *var, char *buff);

static SHOW_VAR warp_status_variables[] = {
    {"Warp_purged_rows", (char *)&show_warp_purged_rows, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_purged_partitions", (char *)&show_warp_purged_partitions, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Warp_purge_failures", (char *)&show_warp_purge_failures, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
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

/* The collations of the strings, by number, looked up once */
static std::atomic<const CHARSET_INFO *> warp_collations[MY_ALL_CHARSETS_SIZE];

static const CHARSET_INFO *warp_collation(uint32_t coll) {
  if(coll >= MY_ALL_CHARSETS_SIZE) {
    return NULL;
  }
  const CHARSET_INFO *cs = warp_collations[coll].load(std::memory_order_acquire);
  if(cs == NULL) {
    cs = get_charset(coll, MYF(0));
    warp_collations[coll].store(cs, std::memory_order_release);
  }
  return cs;
}

/* Compares two strings in a collation with the comparison function of the
   server, the way MySQL compares the strings of a column (with the PAD
   attribute of the collation).  FastBit calls it for the conditions that
   name a collation, see warp_collate_prefix. */
static int warp_string_compare(uint32_t coll, const char *a, size_t alen,
                               const char *b, size_t blen) {
  const CHARSET_INFO *cs = warp_collation(coll);
  if(cs == NULL) {
    /* not a collation of the server, the conditions do not name one */
    return ibis::util::padSpaceCompare(a, alen, b, blen);
  }
  return cs->coll->strnncollsp(cs, pointer_cast<const uchar *>(a), alen,
                               pointer_cast<const uchar *>(b), blen);
}

/* Hashes a string in a collation with the hash function of the server: the
   strings that warp_string_compare finds equal have the same hash value */
static uint64_t warp_string_hash(uint32_t coll, const char *a, size_t alen) {
  const CHARSET_INFO *cs = warp_collation(coll);
  if(cs == NULL) {
    return ibis::util::collationHash(0, a, alen);
  }
  uint64_t nr1 = 1, nr2 = 4;
  cs->coll->hash_sort(cs, pointer_cast<const uchar *>(a), alen, &nr1, &nr2);
  return nr1;
}

/* The collations of the string columns.  FastBit stores a string with a NUL
   at the end, so only the character sets in which a character other than
   NUL has no 0 byte can be stored (not ucs2, utf16 or utf32), and binary
   strings, which may contain any byte, can not.  utf8mb4_bin is compared
   by FastBit itself, the other collations with warp_string_compare.  Only
   the case sensitive and the binary collations are supported. */
static bool warp_collation_supported(const CHARSET_INFO *cs) {
  if(cs == NULL || cs == &my_charset_bin || cs->mbminlen != 1) {
    return false;
  }
  if(cs->number == 46) {
    return true; /* utf8mb4_bin */
  }
  return (cs->state & (MY_CS_BINSORT | MY_CS_CSSORT)) != 0;
}

/* Is the column a binary string (BINARY, VARBINARY or BLOB)?  Its value may
   contain any byte, NUL included, so it is stored encoded, see
   warp_encode_binary. */
static bool warp_is_binary_string(const Field *f) {
  switch(f->real_type()) {
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      return f->charset() == &my_charset_bin;
    default:
      return false;
  }
}

/* Can the strings of the column be stored and compared like MySQL does?
   The binary strings are stored encoded and compared in the binary
   collation, the other strings in their collation. */
static bool warp_string_column_supported(const Field *f) {
  return warp_is_binary_string(f) || warp_collation_supported(f->charset());
}

/* FastBit stores a string with a NUL at the end, so the bytes of a binary
   string are written with 0x01 as an escape: 0x00 as 0x01 0x01, 0x01 as
   0x01 0x02, the other bytes as they are.  The codes of the bytes are in
   the order of the bytes and none is the beginning of another, so the
   encoded strings compare (byte by byte, a shorter string first) and are
   equal like the strings themselves: the comparisons of the binary
   collation are evaluated on the encoded strings. */
static void warp_encode_binary(const char *ptr, size_t len, std::string &out) {
  out.clear();
  out.reserve(len);
  for(size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(ptr[i]);
    if(c <= 1) {
      out += '\x01';
      out += static_cast<char>(c + 1);
    } else {
      out += static_cast<char>(c);
    }
  }
}

static void warp_decode_binary(const std::string &in, std::string &out) {
  out.clear();
  out.reserve(in.size());
  for(size_t i = 0; i < in.size(); ++i) {
    if(in[i] == '\x01' && i + 1 < in.size()) {
      ++i;
      out += static_cast<char>(in[i] - 1);
    } else {
      out += in[i];
    }
  }
}

/* What a FastBit condition on a string column is prefixed with, so that
   FastBit compares the strings in the collation of the column: nothing for
   utf8mb4_bin, which FastBit compares itself (byte by byte, PAD SPACE), and
   collate_N for the collation number N, which FastBit compares with
   warp_string_compare. */
static std::string warp_collate_prefix(const CHARSET_INFO *cs) {
  if(cs->number == 46) {
    return "";
  }
  return "collate_" + std::to_string(cs->number) + " ";
}

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
  ibis::column::setMinIndexRows(my_index_min_rows);
  /* the strings of the collations other than utf8mb4_bin are compared by
     the server */
  ibis::util::setCollationFunctions(warp_string_compare, warp_string_hash);
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
  warp_hton->flags = HTON_CAN_RECREATE;
  warp_hton->partition_flags = warp_partition_flags;
  warp_hton->file_extensions = ha_warp_exts;
  warp_hton->rm_tmp_tables = default_rm_tmp_tables;
  warp_hton->commit = warp_commit;
  warp_hton->rollback = warp_rollback;
  warp_hton->push_to_engine = warp_push_to_engine;
  
  // starts the database and reads in the database state, upgrades
  // tables and does crash recovery
  warp_state = new warp_global_data();
  
  assert(warp_state != NULL);
  warp_state->start_syncer();
  warp_purge_start();
  sql_print_information("WARP storage engine initialization completed");
  DBUG_RETURN(0);
}

static int warp_done_func(void *) {
  sql_print_information("WARP storage engine shutdown started");
  warp_purge_stop();
  /* the commits of warp_sync_commit = OFF are made durable */
  warp_state->stop_syncer();
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
  /* no purge or scan of a partition is running while the directory moves */
  warp_table_write_lock from_lock(from_dir.c_str());
  ibis::fileManager::instance().flushDir(from_dir.c_str());
  ibis::fileManager::instance().flushDir(to_dir.c_str());
  std::string cmd = "mv " + std::string(from) + ".data/ " + std::string(to) + ".data/";
  
  __attribute__((unused))int retval = system(cmd.c_str());
  ibis::fileManager::instance().flushDir(from_dir.c_str());
  ibis::fileManager::instance().flushDir(to_dir.c_str());
  forget_partitions(from);
  forget_partitions(to);
  warp_forget_table_locks(from_dir);
  warp_forget_table_locks(to_dir);
  DBUG_RETURN(0);
}

bool ha_warp::is_deleted(uint64_t rownum) {
  return warp_state->delete_bitmap->is_set(rownum);
}

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

      case MYSQL_TYPE_BIT:
        attribute.append(std::to_string(
            static_cast<ulonglong>((*field)->val_int())).c_str());
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

      /* the bytes of a binary string, NUL included, are written encoded */
      std::string encoded;
      if(warp_is_binary_string(*field)) {
        warp_encode_binary(ptr, attribute.length(), encoded);
        ptr = encoded.data();
        end_ptr = ptr + encoded.size();
      }

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
    /* the memory is not a WARP_SHARE until its members are constructed */
    new (share) WARP_SHARE();

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
    share->~WARP_SHARE();
    my_free(share);
  }
  mysql_mutex_unlock(&warp_mutex);

  DBUG_RETURN(result_code);
}

/* The partitions of a table that is dropped, renamed, truncated or created
   are read from disk again by the next write.  A share can outlive the
   table that it was opened for. */
static void forget_partitions(const char *table_name) {
  mysql_mutex_lock(&warp_mutex);
  const auto range = warp_open_tables->equal_range(table_name);
  for(auto it = range.first; it != range.second; ++it) {
    mysql_mutex_lock(&it->second->mutex);
    it->second->partitions_loaded = false;
    /* the next AUTO_INCREMENT value is read again too */
    it->second->autoinc_loaded = false;
    it->second->autoinc_dirty = false;
    mysql_mutex_unlock(&it->second->mutex);
    /* the values of the UNIQUE indexes are read from disk again */
    std::lock_guard<std::mutex> unique_guard(it->second->unique_mtx);
    it->second->unique_cache_built = false;
    it->second->unique_caches.clear();
  }
  mysql_mutex_unlock(&warp_mutex);
}


#include "warp_purge.h"

static int show_warp_purged_rows(MYSQL_THD, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *reinterpret_cast<longlong *>(buff) = (longlong)warp_purged_rows_total.load();
  return 0;
}

static int show_warp_purged_partitions(MYSQL_THD, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *reinterpret_cast<longlong *>(buff) = (longlong)warp_purged_partitions_total.load();
  return 0;
}

static int show_warp_purge_failures(MYSQL_THD, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *reinterpret_cast<longlong *>(buff) = (longlong)warp_purge_failures_total.load();
  return 0;
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
/* Reads the row of the cursor into buf.  The fields of the table point to
   record[0]: a row that is read into another buffer (record[1] for the row
   of a duplicate key that REPLACE replaces, for example) is read through
   fields that point to it. */
int ha_warp::find_current_row(uchar *buf, ibis::table::cursor *cursor) {
  if(buf == table->record[0]) {
    return find_current_row_impl(buf, cursor);
  }
  repoint_field_to_record(table, table->record[0], buf);
  const int rc = find_current_row_impl(buf, cursor);
  repoint_field_to_record(table, buf, table->record[0]);
  return rc;
}

int ha_warp::find_current_row_impl(uchar *buf, ibis::table::cursor *cursor) {
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
    if(decode_only != nullptr ? bitmap_is_set(decode_only, (*field)->field_index()) :
       (bitmap_is_set(table->read_set, (*field)->field_index()) || current_thd->lex->sql_command == SQLCOM_UPDATE ||  current_thd->lex->sql_command == SQLCOM_UPDATE_MULTI  || current_thd->lex->sql_command == SQLCOM_DELETE || current_thd->lex->sql_command == SQLCOM_DELETE_MULTI )) {
      
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
          if(warp_is_binary_string(*field)) {
            std::string encoded;
            encoded.swap(tmp);
            warp_decode_binary(encoded, tmp);
          }
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
        /* a BIT is stored as the unsigned integer that it holds */
        case MYSQL_TYPE_BIT: {
          uint64_t tmp = 0;
          rc = cursor->getColumnAsULong(cname.c_str(), tmp);
          rc = (*field)->store(static_cast<longlong>(tmp), true);
        } break;

        /* the following are stored as strings in Fastbit */
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_NULL:
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

/* Opening the table (reading the metadata of every partition) to count its
   rows or to estimate a condition costs more than the point query that
   asks for the estimate.  The answer only changes when a partition changes,
   and a partition that changed has another signature (writers rewrite
   -part.txt last).  The signature of a table is the one of its partitions. */
struct warp_table_estimates {
  std::string signature;
  uint64_t rows = 0;
  std::map<std::string, uint64_t> maxima;
};

static std::string warp_table_signature(const std::string &datadir) {
  std::string out;
  DIR *dirp = opendir(datadir.c_str());
  if(dirp == nullptr) return out;
  std::vector<std::string> names;
  struct dirent *ent;
  while((ent = readdir(dirp)) != nullptr) {
    if(ent->d_name[0] == '.' || ent->d_type == DT_REG) continue;
    names.push_back(ent->d_name);
  }
  closedir(dirp);
  std::sort(names.begin(), names.end());
  for(const auto &name : names) {
    const warp_part_signature sig = warp_partition_signature(datadir + "/" + name);
    if(!sig.valid) return std::string();
    char buf[160];
    snprintf(buf, sizeof(buf), "%s:%lu:%ld:%ld.%09ld:%ld:%ld;", name.c_str(),
             (unsigned long)sig.dir_inode, (long)sig.meta_size,
             (long)sig.meta_mtime.tv_sec, (long)sig.meta_mtime.tv_nsec,
             (long)sig.r_size, (long)sig.t_size);
    out += buf;
  }
  return out;
}

/* the rows of the table, and the upper estimate of a condition if where is
   not empty; the caller holds the table lock */
static uint64_t warp_table_estimate(const std::string &datadir,
                                    const std::string &where) {
  static std::mutex registry_mtx;
  static std::unordered_map<std::string, warp_table_estimates> registry;
  const std::string signature = warp_table_signature(datadir);
  /* The estimate of "col = constant" is the one of any other constant: the
     optimizer needs the order of magnitude, and finding it out opens every
     partition of the table.  The other conditions are estimated for the
     constants that they have. */
  std::string shape = where;
  bool equality = !where.empty();
  {
    int comparisons = 0;
    std::string out;
    for(size_t i = 0; i < where.size() && equality; ++i) {
      const char ch = where[i];
      if(ch == '=') ++comparisons;
      if(ch == '<' || ch == '>' || ch == '!' || ch == '\'' || ch == '"' ||
         ch == ',' || ch == '.' || ch == '&' ||
         ch == '|' || ch == '+' || ch == '*' || ch == '/' || ch == '%') {
        equality = false;
      } else if(isdigit((unsigned char)ch) && (i == 0 || !isalnum((unsigned char)where[i - 1]))) {
        while(i + 1 < where.size() && isdigit((unsigned char)where[i + 1])) ++i;
        out += '?';
      } else if(ch == '-' && i + 1 < where.size() && isdigit((unsigned char)where[i + 1])) {
        // the sign belongs to the constant
      } else {
        out += ch;
      }
    }
    if(equality && comparisons == 1 && where.find(" or ") == std::string::npos &&
       where.find(" and ") == std::string::npos &&
       where.find("OR") == std::string::npos && where.find("AND") == std::string::npos) {
      shape = "=:" + out;
    } else {
      equality = false;
    }
  }
  const std::string &key = equality ? shape : where;
  if(!signature.empty()) {
    std::lock_guard<std::mutex> guard(registry_mtx);
    auto it = registry.find(datadir);
    if(it != registry.end() && it->second.signature == signature) {
      if(where.empty()) return it->second.rows;
      auto est = it->second.maxima.find(key);
      if(est != it->second.maxima.end()) return est->second;
    }
  }
  std::unique_ptr<ibis::mensa> table(new ibis::mensa(datadir.c_str()));
  const uint64_t rows = table->nRows();
  uint64_t result = rows;
  if(!where.empty()) {
    uint64_t min = 0, max = 0;
    table->estimate(where.c_str(), min, max);
    result = max;
  }
  if(!signature.empty() && warp_table_signature(datadir) == signature) {
    std::lock_guard<std::mutex> guard(registry_mtx);
    auto &entry = registry[datadir];
    if(entry.signature != signature) {
      entry.signature = signature;
      entry.maxima.clear();
    }
    entry.rows = rows;
    if(!where.empty()) {
      if(entry.maxima.size() >= 256) entry.maxima.clear();
      entry.maxima[key] = result;
    }
  }
  return result;
}

void ha_warp::update_row_count() {
  DBUG_ENTER("ha_warp::row_count");
  /* The table is only opened to count the rows.  It must not be kept: it
     holds the column files of the partitions in the FastBit cache, and as long
     as one connection with the table open does that, the files the writers
     append to can not be removed from the cache (they are "in use") and the
     scans that follow read the old copy with the new number of rows. */
  warp_table_read_lock data_lock(share->data_dir_name);
  stats.records = warp_table_estimate(share->data_dir_name, "");
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
  release_scan_lease();
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

int file_exists(const char *file_name);

/* Reads the partitions of the table and the number of rows in each from
   disk, unless they are already in the share.  Called with share->mutex
   held. */
void ha_warp::load_partition_rows() {
  if(share->partitions_loaded) return;
  share->partition_rows.clear();
  share->next_partition = 0;
  ibis::partList parts;
  ibis::util::gatherParts(parts, share->data_dir_name);
  for(auto part : parts) {
    const char *dir = part->currentDataDir();
    const char *name = dir == NULL ? NULL : strrchr(dir, '/');
    char *end = NULL;
    /* the top level directory is not a partition */
    if(name != NULL && name[1] == 'p' && isdigit((unsigned char)name[2]) &&
       strcmp(dir, share->data_dir_name) != 0) {
      const uint64_t number = strtoull(name + 2, &end, 10);
      if(*end == 0) {
        share->partition_rows[number] = part->nRows();
        share->next_partition = std::max(share->next_partition, number + 1);
      }
    }
    delete part;
  }
  /* a partition that another connection is creating may not be on disk yet */
  if(!share->partitions_writing.empty()) {
    share->next_partition = std::max(share->next_partition,
                                     *share->partitions_writing.rbegin() + 1);
  }
  share->partitions_loaded = true;
}

/* Chooses the partition that the buffered rows are written to and reserves
   it, so that no other connection writes to it at the same time: the first
   partition with room for the rows, or a new partition.  Returns the number
   of the partition.  Called with share->mutex held. */
uint64_t ha_warp::claim_writer_partition() {
  load_partition_rows();
  const uint64_t rows = writer->mRows();
  /* Writing rows to a partition makes its indexes out of date, they are made
     again by the next query, and for a large partition that takes long (a
     second for a million rows).  A small batch of rows therefore goes to a
     partition that is small itself, the tail of the table.  The partitions
     of the tail have no indexes (see warp_index_min_rows), the rows that
     they collect are merged into larger partitions later. */
  const bool small_batch = my_tail_partition_rows > 0 && rows < my_tail_partition_rows;
  const uint64_t tail_rows = std::min<uint64_t>(my_tail_partition_rows, my_partition_max_rows);
  for(const auto &partition : share->partition_rows) {
    if(partition.second + rows > my_partition_max_rows ||
       share->partitions_writing.count(partition.first) != 0 ||
       warp_partition_reserved(share->data_dir_name, partition.first)) {
      continue;
    }
    if(small_batch && partition.second + rows > tail_rows) {
      continue;
    }
    share->partitions_writing.insert(partition.first);
    return partition.first;
  }
  uint64_t number;
  do {
    number = share->next_partition++;
  } while(file_exists((std::string(share->data_dir_name) + "/p" +
                       std::to_string(number)).c_str()));
  share->partition_rows[number] = 0;
  share->partitions_writing.insert(number);
  return number;
}

/* The number of rows in the partitions of the table, the deleted rows
   included.  The partitions are read from disk the first time. */
uint64_t ha_warp::table_row_count() {
  uint64_t rows = 0;
  mysql_mutex_lock(&share->mutex);
  try {
    load_partition_rows();
    for(const auto &partition : share->partition_rows) {
      rows += partition.second;
    }
  } catch(...) {
    /* out of memory reading the partitions: the table counts as empty */
  }
  mysql_mutex_unlock(&share->mutex);
  return rows;
}

/* Write the buffered rows to disk and empty the buffer.  Returns 0 or a
   handler error code.  FastBit reports a lack of memory (typically a
   FastBit cache that is too small) by throwing exceptions, which must not
   reach the server.  share->mutex is only held while a partition is chosen,
   so the other connections that write to the table keep buffering rows
   while the columns are written. */
/* The statements whose rows may be written without waiting for the disk
   (see warp_sync_commit) */
static bool warp_relaxed_statement(THD *thd) {
  switch(thd_sql_command(thd)) {
    case SQLCOM_INSERT:
    case SQLCOM_INSERT_SELECT:
    case SQLCOM_REPLACE:
    case SQLCOM_REPLACE_SELECT:
    case SQLCOM_LOAD:
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
      return true;
    default:
      return false;
  }
}

int ha_warp::write_buffered_rows_to_disk() {
  int rc = 0;
  bool claimed = false;
  bool written_ok = false;
  uint64_t part_number = 0;
  const uint64_t rows = writer->mRows();

  mysql_mutex_lock(&share->mutex);
  try {
    part_number = claim_writer_partition();
    claimed = true;
  } catch(...) {
  }
  mysql_mutex_unlock(&share->mutex);

  try {
    if(!claimed) throw std::bad_alloc();
    const std::string part_name = "p" + std::to_string(part_number);
    const std::string part_dir = std::string(share->data_dir_name) + "/" + part_name;
    /* no scan reads the partition while it is written */
    warp_partition_write_lock data_lock(part_dir, !file_exists(part_dir.c_str()));
    /* The rows of a statement that changes data are not synced with
       warp_sync_commit = OFF: the syncer makes them durable before the
       commit of the transaction (warp_global_data::sync_round).  The other
       writes (ALTER TABLE, CREATE TABLE ... SELECT) are synced. */
    const bool relaxed = !my_sync_commit && warp_relaxed_statement(table->in_use);
    ibis::zfile::setSyncAppends(!relaxed);
    int written = writer->write(part_dir.c_str(), part_name.c_str());
    ibis::zfile::setSyncAppends(true);
    if(relaxed) {
      warp_state->add_pending_partition(part_dir);
      auto current_trx = warp_get_trx(warp_hton, table->in_use);
      if(current_trx != NULL) {
        current_trx->wrote_relaxed = true;
      }
    }
    if(written < 0) {
      sql_print_error("WARP: could not write the buffered rows to %s (error %d)",
                      part_dir.c_str(), written);
      rc = HA_ERR_INTERNAL_ERROR;
    } else {
      written_ok = true;
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

  mysql_mutex_lock(&share->mutex);
  if(claimed) {
    share->partitions_writing.erase(part_number);
    if(written_ok) {
      share->partition_rows[part_number] += rows;
    } else {
      /* the rows on disk are not known, they are read again */
      share->partitions_loaded = false;
    }
  }
  mysql_mutex_unlock(&share->mutex);

  try {
    writer->clearData();
  } catch(...) {
  }
  buffered_rowids.clear();
  save_autoinc();
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
  buffered_rowids.clear();
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
  /* the value of a partition is made by the partitioned table */
  if(table->next_number_field && buf == table->record[0] && !is_partition_child) {
    int error;
    if((error = update_auto_increment())) DBUG_RETURN(error);
  }

  /* UNIQUE and PRIMARY KEY: no other row may have the keys of this one */
  if(has_unique_keys()) {
    const int error = check_unique_keys(buf);
    if(error != 0) DBUG_RETURN(error);
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
  if(has_unique_keys()) {
    remember_unique_keys(buf);
    buffered_rowids.insert(current_rowid);
  }
  autoinc_value_written(buf);
  /* the inserts of a statement that is rolled back alone are deleted, see
     warp_trx::rollback */
  {
    auto current_trx = warp_get_trx(warp_hton, table->in_use);
    if(current_trx != NULL && !current_trx->autocommit) {
      current_trx->write_insert_log_rowid(current_rowid);
    }
  }
  
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
  /* a partition takes at most warp_partition_max_rows rows: FastBit does not
     write the rows of a larger buffer that do not fit */
  if(writer->mRows() >= std::min<uint64_t>(my_write_cache_size, my_partition_max_rows)) {
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
int ha_warp::update_row(const uchar *old_data, uchar *new_data) {
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

  /* the keys of the old row stay locked until the transaction ends: a row
     with one of them can only be inserted once the update is committed or
     rolled back */
  if(has_unique_keys()) {
    const int rc = lock_unique_keys(old_data);
    if(rc != 0) {
      DBUG_RETURN(rc);
    }
  }
  
  // if the write fails (for example due to duplicate key then
  // the statement will be rolled back and the deleted row 
  // will be 
  /* the old row is replaced, it is not a duplicate of the new one */
  unique_exclude_rowid = deleted_rowid;
  int retval = write_row(new_data);
  unique_exclude_rowid = 0;
    
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
int ha_warp::delete_row(const uchar *buf) {
  DBUG_ENTER("ha_warp::delete_row");
  
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  int lock_taken = warp_state->create_lock(current_rowid, current_trx, LOCK_EX);
  /* if deadlock or lock timeout return the error*/
  if(lock_taken != LOCK_EX) {
    DBUG_RETURN(lock_taken);
  }
  /* the keys of the deleted row stay locked until the transaction ends, see
     update_row */
  if(has_unique_keys()) {
    const int rc = lock_unique_keys(buf);
    if(rc != 0) {
      DBUG_RETURN(rc);
    }
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
  /* no purge or scan of a partition is running while the files are removed */
  std::unique_ptr<warp_table_write_lock> data_lock(new warp_table_write_lock(data_dir.c_str()));
  ibis::fileManager::instance().flushDir(data_dir.c_str());
  std::string cmdline =
      std::string("rm -rf ") + std::string(table_name) + ".data/";
  int rc = system(cmdline.c_str());
  ibis::fileManager::instance().flushDir(data_dir.c_str());
  forget_partitions(table_name);
  data_lock.reset();
  warp_forget_table_locks(data_dir);
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
static void warp_release_partition(const char *partition_dir);

/* The number of distinct values of the integer columns of a table, for the
   optimizer: the join of two columns that have no statistics is estimated with
   a default selectivity (10 percent of the pairs of rows), which makes joins of
   large tables hundreds of thousands of times larger than they are.  With
   statistics of an index (the average number of rows per key value) the
   estimate is one over the number of distinct values.  They are estimated from
   a sample of the rows and kept as long as the table does not change. */
struct warp_column_ndv_cache {
  std::string signature;
  uint64_t rows = 0;
  std::map<uint, double> ndv;   // -1: the column has no estimate
  std::chrono::steady_clock::time_point checked;  // the signature was the same then
};
/* The number of distinct values is an estimate that is used for plans: it is
   not checked against the table more often than this */
static const int WARP_NDV_CHECK_SECONDS = 10;

static const size_t WARP_NDV_SAMPLE_ROWS = 1000000;

template <typename T>
static void warp_ndv_collect(const ibis::column *col, size_t stride, std::unordered_map<int64_t, uint32_t> *seen,
                             uint64_t *taken) {
  ibis::array_t<T> values;
  if(col->getValuesArray(&values) != 0) {
    throw std::runtime_error("could not read a column");
  }
  for(size_t i = 0; i < values.size(); i += stride) {
    ++(*seen)[(int64_t)values[i]];
    ++(*taken);
  }
}

/* distinct values of the column, -1.0 if it can not be estimated */
static double warp_estimate_ndv(const std::string &datadir, uint field_index, uint64_t *total_rows) {
  *total_rows = 0;
  ibis::partList parts;
  ibis::util::gatherParts(parts, datadir.c_str(), true);
  std::vector<ibis::part *> work;
  uint64_t rows = 0;
  for(ibis::part *part : parts) {
    if(part != nullptr && part->nRows() > 0 && std::string(part->currentDataDir()) != datadir) {
      work.push_back(part);
      rows += part->nRows();
    }
  }
  *total_rows = rows;
  double result = -1.0;
  if(!work.empty() && rows > 0) {
    /* the partitions that are read: at most eight, spread over the table; the
       rows that are taken: evenly from them, one million at most */
    std::vector<ibis::part *> picked;
    const size_t want = std::min<size_t>(work.size(), 8);
    for(size_t i = 0; i < want; ++i) {
      picked.push_back(work[want == 1 ? 0 : i * (work.size() - 1) / (want - 1)]);
    }
    uint64_t picked_rows = 0;
    for(ibis::part *part : picked) picked_rows += part->nRows();
    const size_t stride = std::max<uint64_t>(1, picked_rows / WARP_NDV_SAMPLE_ROWS);
    std::unordered_map<int64_t, uint32_t> seen;
    uint64_t taken = 0;
    bool supported = true;
    const std::string name = "c" + std::to_string(field_index);
    try {
      for(ibis::part *part : picked) {
        const ibis::column *col = part->getColumn(name.c_str());
        if(col == nullptr) {
          supported = false;
          break;
        }
        switch(col->type()) {
          case ibis::BYTE:   warp_ndv_collect<signed char>(col, stride, &seen, &taken); break;
          case ibis::UBYTE:  warp_ndv_collect<unsigned char>(col, stride, &seen, &taken); break;
          case ibis::SHORT:  warp_ndv_collect<int16_t>(col, stride, &seen, &taken); break;
          case ibis::USHORT: warp_ndv_collect<uint16_t>(col, stride, &seen, &taken); break;
          case ibis::INT:    warp_ndv_collect<int32_t>(col, stride, &seen, &taken); break;
          case ibis::UINT:   warp_ndv_collect<uint32_t>(col, stride, &seen, &taken); break;
          case ibis::LONG:   warp_ndv_collect<int64_t>(col, stride, &seen, &taken); break;
          case ibis::ULONG:  warp_ndv_collect<uint64_t>(col, stride, &seen, &taken); break;
          default: supported = false; break;
        }
        if(!supported) break;
      }
    } catch(...) {
      supported = false;
    }
    if(supported && taken > 0) {
      const double n = (double)taken;
      const double d = (double)seen.size();
      double f1 = 0;
      for(const auto &entry : seen) {
        if(entry.second == 1) f1 += 1;
      }
      const double table_rows = (double)rows;
      if(d == n) {
        result = table_rows;                       // no value was seen twice: a key
      } else {
        /* the estimator of Charikar et al. (GEE) */
        result = std::sqrt(table_rows / n) * f1 + (d - f1);
        result = std::min(std::max(result, d), table_rows);
      }
    }
  }
  for(ibis::part *part : work) {
    warp_release_partition(part->currentDataDir());
  }
  for(auto &part : parts) {
    delete part;
    part = nullptr;
  }
  return result;
}

/* the number of distinct values of the column of the table (-1.0: unknown) and
   the rows of the table */
static double warp_column_ndv(const std::string &datadir, uint field_index, uint64_t *rows) {
  static std::mutex mtx;
  static std::unordered_map<std::string, warp_column_ndv_cache> registry;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> guard(mtx);
    auto it = registry.find(datadir);
    if(it != registry.end() && now - it->second.checked < std::chrono::seconds(WARP_NDV_CHECK_SECONDS)) {
      auto found = it->second.ndv.find(field_index);
      if(found != it->second.ndv.end()) {
        *rows = it->second.rows;
        return found->second;
      }
    }
  }
  const std::string signature = warp_table_signature(datadir);
  if(!signature.empty()) {
    std::lock_guard<std::mutex> guard(mtx);
    auto it = registry.find(datadir);
    if(it != registry.end() && it->second.signature == signature) {
      it->second.checked = now;
      auto found = it->second.ndv.find(field_index);
      if(found != it->second.ndv.end()) {
        *rows = it->second.rows;
        return found->second;
      }
    }
  }
  const double result = warp_estimate_ndv(datadir, field_index, rows);
  if(!signature.empty() && warp_table_signature(datadir) == signature) {
    std::lock_guard<std::mutex> guard(mtx);
    auto &entry = registry[datadir];
    if(entry.signature != signature) {
      entry.signature = signature;
      entry.ndv.clear();
    }
    entry.checked = now;
    entry.rows = *rows;
    entry.ndv[field_index] = result;
  }
  return result;
}

/* the average number of rows per value of the first parts of the keys of the
   table, for the optimizer */
static void warp_set_records_per_key(TABLE *table, const char *datadir) {
  for(uint k = 0; k < table->s->keys; ++k) {
    KEY *key = &table->key_info[k];
    if(!key->supports_records_per_key() || (key->flags & (HA_FULLTEXT | HA_SPATIAL))) {
      continue;
    }
    double distinct = 1.0;
    bool known = true;
    uint64_t rows = 0;
    for(uint j = 0; j < key->actual_key_parts; ++j) {
      if(!known || j >= key->user_defined_key_parts) {
        key->set_records_per_key(j, REC_PER_KEY_UNKNOWN);
        continue;
      }
      const double ndv = warp_column_ndv(datadir, key->key_part[j].field->field_index(), &rows);
      if(ndv < 1.0) {
        known = false;
        key->set_records_per_key(j, REC_PER_KEY_UNKNOWN);
        continue;
      }
      distinct = std::min(distinct * ndv, (double)std::max<uint64_t>(rows, 1));
      float per_key = (float)std::max(1.0, (double)rows / distinct);
      if((key->flags & HA_NOSAME) && j + 1 == key->user_defined_key_parts) {
        per_key = 1.0f;
      }
      key->set_records_per_key(j, per_key);
    }
  }
}

/*
  ::info() is used to return information to the optimizer.
  Currently this table handler doesn't implement most of the fields
  really needed. SHOW also makes use of this data
*/
int ha_warp::info(uint flag) {
  DBUG_ENTER("ha_warp::info");
  close_in_extra = true;
  if(flag & HA_STATUS_ERRKEY) {
    /* the key of the last duplicate, see check_unique_keys */
    errkey = dup_key_nr;
    DBUG_RETURN(0);
  }

  /* The fact table is the WARP table of the statement with the most rows
     (the one with the first data directory if several have as many).  The
     other tables of the statement are the ones it has open; a table that is
     not a WARP table is not a candidate. */
  /* the optimizer has the number of rows per key value of the indexes (a
     join of two columns is estimated with them) */
  if((flag & (HA_STATUS_CONST | HA_STATUS_VARIABLE)) && table->s->keys > 0 && !is_partition_child &&
     table->in_use != nullptr && table->in_use->lex->sql_command == SQLCOM_SELECT) {
    try {
      warp_table_read_lock stats_lock(share->data_dir_name);
      warp_set_records_per_key(table, share->data_dir_name);
    } catch(...) {
      // the keys have no statistics
    }
  }

  const uint64_t own_rows = table_row_count();
  uint64_t least_row_count = own_rows;
  bool is_fact_table = true;
  for(TABLE *cur_table = table->in_use->open_tables; cur_table != NULL;
      cur_table = cur_table->next) {
    ha_warp *other = dynamic_cast<ha_warp *>(cur_table->file);
    if(other == NULL || other->share == NULL || other->share == share) {
      continue;
    }
    const uint64_t rows = other->table_row_count();
    least_row_count = std::min(least_row_count, rows);
    if(rows > own_rows ||
       (rows == own_rows && strcmp(other->share->data_dir_name, share->data_dir_name) < 0)) {
      is_fact_table = false;
    }
  }

  // if this is the fact table set the records to the smallest possible value,
  // the number of rows of the smallest table of the statement plus 2
  // (otherwise const evaluation will be used)
  if(is_fact_table && !is_partition_child &&
     THDVAR(table->in_use, adjust_table_stats_for_joins)) {
    stats.records = least_row_count+2;
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
  }

  /* SHOW TABLE STATUS, SHOW CREATE TABLE, and the value that ALTER TABLE
     gives the copy of the table */
  if(table->found_next_number_field != nullptr) {
    stats.auto_increment_value = autoinc_next_value();
  }

  /* estimate the data size from the record count and average record size */
  stats.data_file_length = stats.mean_rec_length * stats.records;
  
  /* register the table for condition pushdown..  ::info is always called before 
     ::engine_push so this ensures the table information for hybrid join is
     available when we get there
  */
  warp_pushdown_information* pushdown_info = get_or_create_pushdown_info(table->in_use, pd_alias().c_str(), share->data_dir_name);
  pushdown_info->fields = table->s->field;
  set_column_set();

  pushdown_info->column_set = column_set;
  /* a partition is not a table of the joins (see is_partition_child) */
  pushdown_info->is_fact_table = is_fact_table && !is_partition_child;

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
/* What is known about the indexes of a partition: they were made up to date
   when the files of the partition were as signature says.  Looking at every
   index of the partition for every query is much more work than the query
   itself for a table that is read and written all the time. */
struct warp_index_state {
  std::mutex mtx;
  warp_part_signature verified;
};

static std::shared_ptr<warp_index_state> warp_index_state_of(const std::string &datadir) {
  static std::mutex registry_mtx;
  static std::unordered_map<std::string, std::shared_ptr<warp_index_state>> registry;
  std::lock_guard<std::mutex> guard(registry_mtx);
  auto &state = registry[datadir];
  if(!state) state = std::make_shared<warp_index_state>();
  return state;
}

static void warp_maintain_partition_indexes(const char *datadir,
                                            const std::vector<uint> &field_indexes) {
  /* The files of the partition are what they were when its indexes were
     looked at last: nothing to do (rows written to the partition or a
     partition that took its place change the signature) */
  const warp_part_signature before = warp_partition_signature(datadir);
  auto state = warp_index_state_of(datadir);
  /* the scans of the same partition that run at the same time would load,
     rebuild and unload the same index files, the others do not wait for
     them */
  std::lock_guard<std::mutex> maintain_guard(state->mtx);
  if(before.valid && state->verified == before) {
    return;
  }
  std::unique_ptr<ibis::part> tbl(new ibis::part(datadir));
  if(tbl->nRows() < my_index_min_rows) {
    /* the partition is scanned: its old index files are of no use and would
       have to be made again after every write to the partition */
    for (uint field_index : field_indexes) {
      std::string columnIndexFilename = std::string(datadir) + "/c" + std::to_string(field_index) + ".idx";
      if(file_exists(columnIndexFilename)) {
        auto col = tbl->getColumn(field_index);
        if(col != NULL) col->purgeIndexFile();
        ibis::fileManager::instance().flushFile(columnIndexFilename.c_str());
      }
    }
    if(before.valid && warp_partition_signature(datadir) == before) {
      state->verified = before;
    }
    return;
  }
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
  /* remembered if the partition did not change meanwhile */
  if(before.valid && warp_partition_signature(datadir) == before) {
    state->verified = before;
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
  const std::string where = scan_where;
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
    if((*it)->nRows() < my_prefetch_min_rows) {
      continue; // a thread costs more than reading it here
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
void ha_warp::acquire_scan_lease() {
  if(scan_lease) return;
  scan_lease = warp_table_locks(share->data_dir_name);
  warp_scan_enter(scan_lease);
}

void ha_warp::release_scan_lease() {
  if(!scan_lease) return;
  warp_scan_leave(scan_lease);
  scan_lease.reset();
}

void ha_warp::release_scanned_table() {
  if(filtered_table != NULL) {
    delete base_table;
    base_table = NULL;
    if(scan_liga_part != nullptr) {
      /* the partition object outlives the table; the indexes that the
         select loaded must not stay in memory until the scan ends */
      scan_liga_part->unloadIndexes();
      scan_liga_part = nullptr;
    }
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
  /* REPLACE and INSERT ... ON DUPLICATE KEY UPDATE read the row of a
     duplicate key, see check_unique_keys */
  if(xtra == HA_EXTRA_WRITE_CAN_REPLACE || xtra == HA_EXTRA_INSERT_WITH_UPDATE) {
    dup_row_is_read = true;
  } else if(xtra == HA_EXTRA_WRITE_CANNOT_REPLACE) {
    dup_row_is_read = false;
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
  par_join.reset();
  par_agg.reset();
  if(index_scan_open) {
    end_scan(true);
  }
  index_base_where = "";
  // free up memory used for pushdown filters
  auto pushdown_info = get_pushdown_info(table->in_use, pd_alias().c_str());
  
  pushdown_mtx.lock();
  auto it = pd_info.find(table->in_use);
  if(it != pd_info.end()) {
  
    // remove the pushdown info for this table
    auto it2 = it->second->find(pd_alias().c_str());
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
  scan_where = "";
  outer_params.clear();
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
        /* the strings are compared in the collation of the column, see
           warp_string_column_supported */
        if(!warp_string_column_supported(*field)) {
          std::string errmsg = "Unsupported character set or collation for column (only binary strings and case sensitive and binary collations of character sets without NUL bytes are supported): " +
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
        /* up to 64 bits, stored as the unsigned integer that it holds */
        index_spec="<binning none/><encoding binary/>";
        datatype = ibis::ULONG;
        break;

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

int ha_warp::create(const char *name, TABLE *table_arg,
                    HA_CREATE_INFO *create_info, dd::Table *) {
  DBUG_ENTER("ha_warp::create");
  int rc = 0;
  /* the indexes are declarations, see ha_warp::make_key_condition */
  for(uint k = 0; k < table_arg->s->keys; ++k) {
    const KEY *key = &table_arg->key_info[k];
    /* UNIQUE and PRIMARY KEY are checked when rows are written, see
       check_unique_keys */
    if(key->flags & (HA_FULLTEXT | HA_SPATIAL)) {
      my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), "WARP",
               (key->flags & HA_FULLTEXT) ? "FULLTEXT index"
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
  /* CREATE TABLE ... AUTO_INCREMENT = n, and the next value of a table that
     ALTER TABLE copies */
  if(rc == 0 && create_info != NULL && create_info->auto_increment_value > 1) {
    FILE *f = fopen(warp_autoinc_file(share->data_dir_name).c_str(), "w");
    if(f != NULL) {
      const uint64_t next = create_info->auto_increment_value;
      fwrite(&next, sizeof(next), 1, f);
      fclose(f);
    }
  }
  forget_partitions(name);

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
  /* the rows that nobody can see are removed first, whatever their number */
  const uint64_t purged = warp_purge_table(share->data_dir_name, 0);
  if(purged > 0) {
    sql_print_information("WARP: OPTIMIZE removed %llu rows of %s",
                          (unsigned long long)purged, share->data_dir_name);
  }
  /* and the partitions that were written in small batches are merged */
  const uint64_t merged = warp_merge_tail_partitions(share->data_dir_name);
  if(merged > 0) {
    sql_print_information("WARP: OPTIMIZE merged %llu partitions of %s",
                          (unsigned long long)merged, share->data_dir_name);
  }
  mysql_mutex_lock(&share->mutex);
  ibis::partList parts;
  ibis::util::gatherParts(parts, share->data_dir_name, true);
  for (auto part : parts) {
    const char *dir = part->currentDataDir();
    if (dir == nullptr || part->nRows() == 0) continue;
    /* the column files of a partition are not rewritten while rows are
       written to them or while they are read */
    warp_partition_write_lock data_lock(dir);
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
  /* A UNIQUE or PRIMARY KEY that is added is not in the list: the table is
     copied, the copy checks that the rows have different keys */
  static const Alter_inplace_info::HA_ALTER_FLAGS index_changes =
      Alter_inplace_info::ADD_INDEX | Alter_inplace_info::DROP_INDEX |
      Alter_inplace_info::DROP_UNIQUE_INDEX | Alter_inplace_info::DROP_PK_INDEX |
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
    if(key->flags & (HA_FULLTEXT | HA_SPATIAL)) {
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
/* The files of a partition as one string, empty if they can not be looked at */
static std::string warp_partition_signature_string(const std::string &dir) {
  const warp_part_signature sig = warp_partition_signature(dir);
  if(!sig.valid) return std::string();
  char buf[160];
  snprintf(buf, sizeof(buf), "%lu:%ld:%ld.%09ld:%ld:%ld",
           (unsigned long)sig.dir_inode, (long)sig.meta_size,
           (long)sig.meta_mtime.tv_sec, (long)sig.meta_mtime.tv_nsec,
           (long)sig.r_size, (long)sig.t_size);
  return buf;
}

static std::unordered_map<std::string, std::string>
warp_partition_signature_strings(const std::string &datadir) {
  std::unordered_map<std::string, std::string> out;
  DIR *dirp = opendir(datadir.c_str());
  if(dirp == nullptr) return out;
  struct dirent *ent;
  while((ent = readdir(dirp)) != nullptr) {
    if(ent->d_name[0] == '.' || ent->d_type == DT_REG) continue;
    const std::string dir = datadir + "/" + ent->d_name;
    const std::string sig = warp_partition_signature_string(dir);
    if(!sig.empty()) out[dir] = sig;
  }
  closedir(dirp);
  return out;
}

int ha_warp::rnd_init(bool) {
  DBUG_ENTER("ha_warp::rnd_init");
  if(star_agg) {
    /* the query is aggregated by the engine, the rows are the groups */
    DBUG_RETURN(star_agg_run() ? HA_ERR_INTERNAL_ERROR : 0);
  }
  if(par_agg) {
    /* the rows of the table are aggregated by the engine, the rows are the groups */
    DBUG_RETURN(pa_run());
  }
  if(par_join) {
    /* the tables of the query are joined by the engine, the rows are the
       joined rows */
    DBUG_RETURN(pj_run());
  }
  if(pj_following()) {
    /* a table of the join of the engine: one row for every probe row */
    pj_follower_pending = true;
    DBUG_RETURN(0);
  }
  if(!index_scan_mode && index_scan_open) {
    /* the scan of an index lookup that the statement left open */
    end_scan(false);
  }
  discard_prefetched();
  fetch_count = 0;
  auto pushdown_info = get_pushdown_info(table->in_use, pd_alias().c_str());
  if(pushdown_info == NULL) {
    /* ::info creates the pushdown information while a statement is
       optimized.  A replica applying row events looks up rows without it. */
    pushdown_info = get_or_create_pushdown_info(table->in_use, pd_alias().c_str(), share->data_dir_name);
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
  make_scan_where();
  
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
 
      if((filter_it->first)->dim_alias == pd_alias()) {
        auto tmp = (filter_it->first);
        current_matching_dim_ridset_it = tmp->get_rownums()->begin();
 
        current_matching_dim_ridset = tmp->get_rownums();
        
        break;
      }
    }
    rownum = 0;
    
  } else {
    base_table = NULL;
    /* the partitions that the scan lists are not merged until it ends */
    acquire_scan_lease();
    /* The partitions are found by reading their metadata, which a writer that
       appends to a partition rewrites.  A partition whose metadata is read
       at that moment is not found, the scan then returns no rows at all. */
    warp_table_read_lock partitions_lock(share->data_dir_name);
    /* a scan that the join of the engine makes of a table (warp_par_join.h) is a
       plain scan: the join pushdown of the largest table of the statement
       (which it would be here) computes a join of its own */
    if( (!is_partition_child && !pj_plain_scan && get_pushdown_info_count(current_thd) > 1 &&
         pushdown_info->is_fact_table) || partition_filter_partition_name != "" ) {
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
      /* taken before the partitions are read: a partition that still has
         this signature when it is scanned has not changed since */
      scan_part_sigs = warp_partition_signature_strings(share->data_dir_name);
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
  if(column_query->evaluate() < 0) {
    throw std::runtime_error(std::string("could not evaluate the pushed down condition on ") +
                             (*part_it)->currentDataDir());
  }
  if(column_query->getNumHits() != 0) {
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
  if(par_agg) {
    DBUG_RETURN(pa_next(buf));
  }
  if(par_join) {
    DBUG_RETURN(pj_next(buf));
  }
  if(pj_following()) {
    DBUG_RETURN(pj_follower_next(buf));
  }
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
  /* a statement that is killed stops at the next row */
  if(thd_killed(ha_thd())) {
    DBUG_RETURN(HA_ERR_QUERY_INTERRUPTED);
  }
  
  if( !full_partition_scan && partitions != NULL && bitmap_merge_join_executed == false ) {
    use_thread_budget();
    join_read_lock.emplace(share->data_dir_name);
    if( std::string((*part_it)->currentDataDir()) == std::string(share->data_dir_name) ) {
      ++part_it;
    }

    /* Whether the join of the table is pushed down is known before the
       partitions are looked at.  Without a pushed down join the condition is
       evaluated when a partition is read (in the threads that read ahead),
       evaluating it here for every partition one after the other would be
       done twice and by one thread. */
    bitmap_merge_join();
    const bool no_pushed_join = fact_table_filters.size() == 0;
    if(no_pushed_join && push_where_clause == "") {
      push_where_clause = "1=1";
    }
  
    while( !no_pushed_join && part_it != partitions->end() ) {        
      /* a statement that is killed schedules no more joins, the running
         ones are waited for below */
      if(thd_killed(ha_thd())) {
        break;
      }
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
      /* the worker evaluates the condition (exec_pushdown_join), the
         partitions are evaluated at the same time */
      {
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
            if( join_error.load() != 0 || thd_killed(ha_thd()) ) {
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

  /* the joins of a killed statement were not all scheduled */
  if(thd_killed(ha_thd())) {
    DBUG_RETURN(HA_ERR_QUERY_INTERRUPTED);
  }

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
        warp_partition_read_lock data_lock(find_it->first);
        delete base_table;
        base_table = ibis::table::create(find_it->first.c_str());
        assert(base_table != NULL);
            
        // this will do some IO to read in projected columns that where not used for filters
        maintain_indexes(find_it->first.c_str());
        filtered_table = base_table->select(column_set.c_str(), scan_where.c_str());
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
        warp_partition_read_lock data_lock((*part_it)->currentDataDir());
        delete base_table;
        const std::string part_dir((*part_it)->currentDataDir());
        auto known = scan_part_sigs.find(part_dir);
        if(known != scan_part_sigs.end() &&
           known->second == warp_partition_signature_string(part_dir)) {
          /* the partition is as it was when the scan read its metadata */
          base_table = new ibis::liga(**part_it);
          scan_liga_part = *part_it;
        } else {
          base_table = ibis::table::create(part_dir.c_str());
        }
        assert(base_table != NULL);
      
        maintain_indexes((*part_it)->currentDataDir());
        filtered_table = base_table->select(column_set.c_str(), scan_where.c_str());
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
  auto pushdown_info = get_pushdown_info(table->in_use, pd_alias().c_str());
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
    /* a statement that is killed stops at the next partition */
    if(thd_killed(ha_thd())) {
      ok = false;
      break;
    }
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
  if(thd_killed(ha_thd())) {
    return HA_ERR_QUERY_INTERRUPTED;
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

  /* the scan of the joined rows has nothing else to release */
  pj_close();

  /* workers use members of this handler (and the part objects freed below) */
  wait_for_join_threads();
  discard_prefetched();
  join_error = 0;
  
  blobroot.Clear();

  /* The condition that was pushed down stays: the table of a subquery or of
     the inner side of a nested loop is scanned again for every outer row, and
     MySQL has removed the pushed condition from its own filter.  It is reset
     when the statement ends (ha_warp::reset). */

  if(!scan_uses_pushdown_tables) {
    if(cursor) delete cursor;
    if(filtered_table) delete filtered_table;
    if(base_table) delete base_table;
  }
  scan_uses_pushdown_tables = false;

  scan_liga_part = nullptr;
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
      /* write_buffered_rows_to_disk flushed the partition that was written.
         The files of the other partitions did not change, flushing them
         too makes the next query read and decompress them again (and
         fails for the files that other queries use). */
      ibis::fileManager::instance().flushFile(
          (std::string(share->data_dir_name) + "/-part.txt").c_str());
    }
  }
  index_scan_open = false;
  release_scan_lease();

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
  /* The columns of the statement: rnd_pos can be used without a scan of
     this handler, or after a scan that read other columns (INSERT ... ON
     DUPLICATE KEY UPDATE reads every column of the duplicate) */
  set_column_set();
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
  /* a virtual generated column (the hidden column of a functional index,
     for example) is stored like the other columns, see table_flags */
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
      /* FastBit compares the strings in the collation of the column */
      return warp_string_column_supported(f);
    default:
      return false;
  }
}

/* Writes the bytes of a string as a quoted FastBit string literal, the
   bytes of a binary string encoded the way they are stored (see
   warp_encode_binary).  Returns false for another string that contains a
   NUL: FastBit strings end at a NUL. */
static bool warp_quote_string(const char *ptr, size_t len, bool binary,
                              std::string &quoted) {
  std::string encoded;
  if(binary) {
    warp_encode_binary(ptr, len, encoded);
    ptr = encoded.data();
    len = encoded.size();
  }
  std::string escaped;
  for(size_t i = 0; i < len; ++i) {
    const char c = ptr[i];
    if(c == 0) {
      return false;
    } else if(c == '\'') {
      escaped += "\\'";
    } else if(c == '\\') {
      escaped += "\\\\";
    } else {
      escaped += c;
    }
  }
  quoted = "'" + escaped + "'";
  return true;
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
      ok = warp_quote_string(val->ptr(), val->length(),
                             warp_is_binary_string(f), v.text);
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
        v.text = std::to_string(warp_encode_time(t)) + "U";
      }
    } else if(warp_is_timestamp_type(f->real_type())) {
      v.text = std::to_string(warp_timestamp_field_value(f)) + "U";
    } else {
      Datetime_val dt;
      if(f->val_datetime(&dt, TIME_DATETIME_ONLY)) {
        ok = false;
      } else {
        /* about 2^60: a double can not hold it exactly */
        v.text = std::to_string(TIME_to_longlong_datetime_packed(dt)) + "U";
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
  /* a string is compared in the collation of the column */
  const std::string collate =
      (f->real_type() == MYSQL_TYPE_VARCHAR ||
       f->real_type() == MYSQL_TYPE_STRING)
          ? warp_collate_prefix(f->charset())
          : "";
  const std::string cmp = collate + "c" + pos + " " + op + " " + v.text;
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

/* ---------------------------------------------------------------------
   UNIQUE and PRIMARY KEY

   A row may not have the key of another row that exists: one that is
   committed, or that this transaction wrote, and that is not deleted.  This
   is the latest state of the table, not the one that the transaction reads
   (a row that was committed after the transaction began is a duplicate
   too), like InnoDB checks.

   Two transactions must not insert the same key at the same time, so the
   key is locked before it is looked for: a lock on a number made from the
   table, the index and the value of the key (the row locks are on row ids,
   which are below 2^63, the key locks have the high bit set).  The lock is
   held until the transaction ends.  A delete or an update locks the keys of
   the row that it removes too, so a row with one of them is inserted once
   the change is committed (the key is free) or rolled back (it is not).
   With the key locked, a row with the key is either committed or of this
   transaction.

   NULL is not equal to NULL: a key with a NULL part is never a duplicate.
   --------------------------------------------------------------------- */

bool ha_warp::has_unique_keys() const {
  for(uint k = 0; k < table->s->keys; ++k) {
    if(table->key_info[k].flags & HA_NOSAME) {
      return true;
    }
  }
  return false;
}

/* The values of an index are kept by the columns of the index, the number
   of an index changes when an index is added or dropped in place */
std::string ha_warp::unique_key_columns(uint key_nr) const {
  const KEY *key = &table->key_info[key_nr];
  std::string columns;
  for(uint p = 0; p < key->user_defined_key_parts; ++p) {
    columns += std::to_string(key->key_part[p].field->field_index()) + ",";
  }
  return columns;
}

bool ha_warp::unique_key_is_null(uint key_nr, const uchar *record) const {
  const KEY *key = &table->key_info[key_nr];
  const ptrdiff_t diff = record - table->record[0];
  for(uint p = 0; p < key->user_defined_key_parts; ++p) {
    if(key->key_part[p].field->is_null(diff)) {
      return true;
    }
  }
  return false;
}

/* The key of a row as a string that compares byte by byte like the values
   compare in the collations of the columns ('a' and 'a ' are the same in a
   PAD SPACE collation): the sort key of every part, then its length, like
   the write sets of replication make them.  Returns false for a key with a
   NULL part, which is never a duplicate. */
bool ha_warp::unique_key_image(uint key_nr, const uchar *record,
                               std::string *image) const {
  const KEY *key = &table->key_info[key_nr];
  const ptrdiff_t diff = record - table->record[0];
  image->clear();
  std::vector<uchar> part;
  for(uint p = 0; p < key->user_defined_key_parts; ++p) {
    const KEY_PART_INFO *kp = &key->key_part[p];
    Field *f = kp->field;
    if(f->is_null(diff)) {
      return false;
    }
    const CHARSET_INFO *cs = f->charset();
    const size_t max_length = cs->coll->strnxfrmlen(cs, kp->length);
    part.assign(max_length + 1, 0);
    f->move_field_offset(diff);
    const size_t length =
        f->make_sort_key(part.data(), max_length, kp->length / cs->mbmaxlen);
    f->move_field_offset(-diff);
    image->append(pointer_cast<const char *>(part.data()), length);
    const uint16_t length16 = static_cast<uint16_t>(length);
    image->append(pointer_cast<const char *>(&length16), sizeof(length16));
  }
  return true;
}

/* The lock of a key: a hash of the data directory, the index and the
   values of the key, which the collations of the columns hash (equal
   strings, 'a' and 'a ' in a PAD SPACE collation for example, have the same
   hash) */
uint64_t ha_warp::unique_key_lock(uint key_nr, const uchar *record) const {
  uint64_t nr1 = 1, nr2 = 4;
  my_charset_bin.coll->hash_sort(&my_charset_bin,
                                 pointer_cast<const uchar *>(share->data_dir_name),
                                 strlen(share->data_dir_name), &nr1, &nr2);
  ulong h1 = static_cast<ulong>(nr1) ^ (key_nr + 1) * 0x9E3779B97F4A7C15ULL;
  ulong h2 = static_cast<ulong>(nr2);
  const KEY *key = &table->key_info[key_nr];
  const ptrdiff_t diff = record - table->record[0];
  for(uint p = 0; p < key->user_defined_key_parts; ++p) {
    Field *f = key->key_part[p].field;
    f->move_field_offset(diff);
    f->hash(&h1, &h2);
    f->move_field_offset(-diff);
  }
  const uint64_t h = static_cast<uint64_t>(h1) * 0xC2B2AE3D27D4EB4FULL ^
                     static_cast<uint64_t>(h2);
  return (1ULL << 63) | (h & ((1ULL << 63) - 1));
}

static int warp_key_lock_error(int lock_taken) {
  if(lock_taken == LOCK_DEADLOCK) return HA_ERR_LOCK_DEADLOCK;
  return HA_ERR_LOCK_WAIT_TIMEOUT;
}

int ha_warp::lock_unique_keys(const uchar *record) {
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  for(uint k = 0; k < table->s->keys; ++k) {
    if(!(table->key_info[k].flags & HA_NOSAME) ||
       unique_key_is_null(k, record)) {
      continue;
    }
    const int lock_taken = warp_state->create_lock(unique_key_lock(k, record),
                                                   current_trx, LOCK_EX);
    if(lock_taken != LOCK_EX) {
      return warp_key_lock_error(lock_taken);
    }
  }
  return 0;
}

/* Does the row with the key exist (see above)?  The key is locked, so the
   transaction that inserted it is this one or it has ended. */
bool ha_warp::unique_row_exists(uint64_t rowid, uint64_t trx_id) {
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  assert(current_trx != NULL);
  /* the row that an update replaces */
  if(rowid == unique_exclude_rowid) return false;
  /* committed deletes, and the inserts of rolled back statements */
  if(warp_state->delete_bitmap->is_set(rowid)) return false;
  /* deleted by this transaction */
  if(warp_state->get_history_lock(rowid) == current_trx->trx_id) return false;
  /* the rows of rolled back transactions do not exist */
  return trx_id == current_trx->trx_id ||
         (trx_id != 0 && warp_state->is_trx_committed(trx_id));
}

/* Reads the keys of the UNIQUE indexes of every row on disk into the cache
   of the share.  Called with unique_mtx held. */
bool ha_warp::build_unique_cache() {
  share->unique_caches.clear();
  MY_BITMAP key_columns;
  if(bitmap_init(&key_columns, nullptr, table->s->fields)) {
    return false;
  }
  std::string columns = "r,t";
  for(uint k = 0; k < table->s->keys; ++k) {
    const KEY *key = &table->key_info[k];
    if(!(key->flags & HA_NOSAME)) continue;
    share->unique_caches[unique_key_columns(k)];
    for(uint p = 0; p < key->user_defined_key_parts; ++p) {
      const Field *f = key->key_part[p].field;
      if(bitmap_test_and_set(&key_columns, f->field_index())) continue;
      columns += ",c" + std::to_string(f->field_index());
      if(f->is_nullable()) columns += ",n" + std::to_string(f->field_index());
    }
  }
  std::vector<uchar> scratch(table->s->rec_buff_length);
  memcpy(scratch.data(), table->s->default_values, table->s->reclength);
  bool ok = true;
  std::string image;
  try {
    warp_table_read_lock data_lock(share->data_dir_name);
    ibis::table *base = ibis::mensa::create(share->data_dir_name);
    if(base != NULL && base->nRows() > 0) {
      ibis::table *rows = base->select(columns.c_str(), "1=1");
      ibis::table::cursor *cursor = rows != NULL ? rows->createCursor() : NULL;
      ok = cursor != NULL;
      decode_only = &key_columns;
      while(ok && cursor->fetch() == 0) {
        uint64_t r = 0, t = 0;
        cursor->getColumnAsULong("r", r);
        cursor->getColumnAsULong("t", t);
        /* the committed deletes are not keys of any row */
        if(warp_state->delete_bitmap->is_set(r)) continue;
        if(find_current_row(scratch.data(), cursor) != 0) {
          ok = false;
          break;
        }
        for(uint k = 0; k < table->s->keys; ++k) {
          if((table->key_info[k].flags & HA_NOSAME) &&
             unique_key_image(k, scratch.data(), &image)) {
            share->unique_caches[unique_key_columns(k)].sorted.push_back(
                warp_unique_entry{image, r, t});
          }
        }
      }
      decode_only = nullptr;
      delete cursor;
      delete rows;
    }
    delete base;
  } catch(...) {
    decode_only = nullptr;
    ok = false;
  }
  bitmap_free(&key_columns);
  if(!ok) {
    share->unique_caches.clear();
    return false;
  }
  for(auto &cache : share->unique_caches) {
    std::sort(cache.second.sorted.begin(), cache.second.sorted.end());
  }
  share->unique_cache_built = true;
  return true;
}

/* The rows that are written are added to the cache, buffered or not */
void ha_warp::remember_unique_keys(const uchar *record) {
  auto current_trx = warp_get_trx(warp_hton, table->in_use);
  std::string image;
  std::lock_guard<std::mutex> guard(share->unique_mtx);
  if(!share->unique_cache_built) {
    return; /* it is read from disk, the row is there */
  }
  for(uint k = 0; k < table->s->keys; ++k) {
    if(!(table->key_info[k].flags & HA_NOSAME) ||
       !unique_key_image(k, record, &image)) {
      continue;
    }
    warp_unique_cache &cache = share->unique_caches[unique_key_columns(k)];
    cache.recent.emplace(image, std::make_pair(current_rowid,
                                               current_trx->trx_id));
    /* the recent keys are merged into the sorted ones from time to time */
    if(cache.recent.size() >= 65536) {
      const size_t old_size = cache.sorted.size();
      for(const auto &entry : cache.recent) {
        cache.sorted.push_back(warp_unique_entry{entry.first,
                                                 entry.second.first,
                                                 entry.second.second});
      }
      std::inplace_merge(cache.sorted.begin(), cache.sorted.begin() + old_size,
                         cache.sorted.end());
      cache.recent.clear();
    }
  }
}

/* ---------------------------------------------------------------------
   UNIQUE and PRIMARY KEY, the check

   The key of the row is looked for in the cache of the values of the index
   (a binary search), which has the rows on disk and the ones that are
   buffered: nothing is read from disk and no row is written to look for a
   key.  The rows that have the key are looked at, the first one that
   exists is the duplicate.
   --------------------------------------------------------------------- */
int ha_warp::check_unique_keys(uchar *record) {
  int rc = lock_unique_keys(record);
  if(rc != 0) {
    return rc;
  }
  std::string image;
  std::unique_lock<std::mutex> guard(share->unique_mtx);
  if(!share->unique_cache_built && !build_unique_cache()) {
    return HA_ERR_INTERNAL_ERROR;
  }
  for(uint k = 0; k < table->s->keys; ++k) {
    const KEY *key = &table->key_info[k];
    if(!(key->flags & HA_NOSAME) || !unique_key_image(k, record, &image)) {
      continue;
    }
    warp_unique_cache &cache = share->unique_caches[unique_key_columns(k)];
    bool found = false;
    uint64_t dup_rowid = 0;
    const warp_unique_entry probe{image, 0, 0};
    auto range = std::equal_range(cache.sorted.begin(), cache.sorted.end(),
                                  probe);
    for(auto it = range.first; !found && it != range.second; ++it) {
      if(unique_row_exists(it->rowid, it->trx_id)) {
        found = true;
        dup_rowid = it->rowid;
      }
    }
    auto recent = cache.recent.equal_range(image);
    for(auto it = recent.first; !found && it != recent.second; ++it) {
      if(unique_row_exists(it->second.first, it->second.second)) {
        found = true;
        dup_rowid = it->second.first;
      }
    }
    if(found) {
      guard.unlock();
      errkey = dup_key_nr = k;
      my_store_ptr(dup_ref, ref_length, dup_rowid);
      /* The server reads the row of the duplicate by its position (see
         HA_DUPLICATE_POS) for REPLACE and INSERT ... ON DUPLICATE KEY
         UPDATE: a row of this statement that is still buffered is written
         so that it can be read. */
      if(dup_row_is_read && buffered_rowids.count(dup_rowid) != 0) {
        rc = write_buffered_rows_to_disk();
        if(rc != 0) {
          return rc;
        }
      }
      return HA_ERR_FOUND_DUPP_KEY;
    }
  }
  return 0;
}

/* ---------------------------------------------------------------------
   AUTO_INCREMENT

   The next value of the table is in the share.  It is read when it is
   first needed: the value of the auto_increment file in the data directory
   (written by CREATE TABLE ... AUTO_INCREMENT = n and whenever values are
   given out), or the largest value of the column on disk plus one if that
   is larger.  A statement is given the values it asks for, the ones it did
   not use are given back when it ends if no other statement took values
   since.  A value that is written (given in the statement, or by an update)
   and is not smaller than the next one moves the next one after it.
   --------------------------------------------------------------------- */

static std::string warp_autoinc_file(const char *data_dir) {
  return std::string(data_dir) + "/auto_increment";
}

void ha_warp::load_autoinc() {
  mysql_mutex_lock(&share->mutex);
  const bool loaded = share->autoinc_loaded;
  mysql_mutex_unlock(&share->mutex);
  if(loaded) {
    return;
  }
  /* the column is read without share->mutex, which the purge takes while it
     holds the lock of the table */
  uint64_t next = 1;
  FILE *f = fopen(warp_autoinc_file(share->data_dir_name).c_str(), "r");
  if(f != NULL) {
    uint64_t value = 0;
    if(fread(&value, sizeof(value), 1, f) == 1 && value > next) {
      next = value;
    }
    fclose(f);
  }
  Field *field = table->found_next_number_field;
  if(field != nullptr) {
    try {
      warp_table_read_lock data_lock(share->data_dir_name);
      ibis::table *base = ibis::mensa::create(share->data_dir_name);
      if(base != NULL && base->nRows() > 0) {
        const std::string column = "c" + std::to_string(field->field_index());
        ibis::table *rows = base->select(column.c_str(), "1=1");
        ibis::table::cursor *cursor = rows != NULL ? rows->createCursor() : NULL;
        while(cursor != NULL && cursor->fetch() == 0) {
          uint64_t value = 0;
          if(field->is_unsigned()) {
            cursor->getColumnAsULong(column.c_str(), value);
          } else {
            int64_t signed_value = 0;
            cursor->getColumnAsLong(column.c_str(), signed_value);
            value = signed_value > 0 ? static_cast<uint64_t>(signed_value) : 0;
          }
          if(value != UINT64_MAX && value + 1 > next) {
            next = value + 1;
          }
        }
        delete cursor;
        delete rows;
      }
      delete base;
    } catch(...) {
    }
  }
  mysql_mutex_lock(&share->mutex);
  if(!share->autoinc_loaded) {
    share->autoinc_next = next;
    share->autoinc_loaded = true;
  }
  mysql_mutex_unlock(&share->mutex);
}

/* Writes the next value to the auto_increment file if it changed */
void ha_warp::save_autoinc() {
  mysql_mutex_lock(&share->mutex);
  if(share->autoinc_dirty) {
    FILE *f = fopen(warp_autoinc_file(share->data_dir_name).c_str(), "w");
    if(f != NULL) {
      fwrite(&share->autoinc_next, sizeof(share->autoinc_next), 1, f);
      fclose(f);
      share->autoinc_dirty = false;
    }
  }
  mysql_mutex_unlock(&share->mutex);
}

uint64_t ha_warp::autoinc_next_value() {
  load_autoinc();
  mysql_mutex_lock(&share->mutex);
  const uint64_t next = share->autoinc_next;
  mysql_mutex_unlock(&share->mutex);
  return next;
}

/* A row was written: its value of the column moves the next value */
void ha_warp::autoinc_value_written(const uchar *record) {
  Field *field = table->found_next_number_field;
  if(field == nullptr) {
    return;
  }
  const ptrdiff_t diff = record - table->record[0];
  if(field->is_null(diff)) {
    return;
  }
  field->move_field_offset(diff);
  const longlong value = field->val_int();
  field->move_field_offset(-diff);
  if(!field->is_unsigned() && value <= 0) {
    return;
  }
  const uint64_t u = static_cast<uint64_t>(value);
  load_autoinc();
  mysql_mutex_lock(&share->mutex);
  if(u != UINT64_MAX && u + 1 > share->autoinc_next) {
    share->autoinc_next = u + 1;
    share->autoinc_dirty = true;
  }
  mysql_mutex_unlock(&share->mutex);
}

void ha_warp::get_auto_increment(ulonglong offset, ulonglong increment,
                                 ulonglong nb_desired_values,
                                 ulonglong *first_value,
                                 ulonglong *nb_reserved_values) {
  load_autoinc();
  const uint64_t step = increment > 0 ? increment : 1;
  const uint64_t count = nb_desired_values > 0 ? nb_desired_values : 1;
  mysql_mutex_lock(&share->mutex);
  /* the first value of the series of auto_increment_offset and
     auto_increment_increment from the next value on (the offset is not used
     when it is larger than the increment) */
  uint64_t first = share->autoinc_next;
  if(step > 1) {
    const uint64_t start = (offset > 0 && offset <= step) ? offset : 1;
    if(first <= start) {
      first = start;
    } else if(first - start > UINT64_MAX - step) {
      first = UINT64_MAX;
    } else {
      first = start + ((first - start + step - 1) / step) * step;
    }
  }
  uint64_t end = UINT64_MAX;
  if(first != UINT64_MAX && count <= (UINT64_MAX - first) / step) {
    end = first + count * step;
  }
  share->autoinc_next = end;
  share->autoinc_dirty = true;
  mysql_mutex_unlock(&share->mutex);
  autoinc_reserved_end = end;
  save_autoinc();
  *first_value = first;
  *nb_reserved_values = count;
}

/* SHOW CREATE TABLE, and ALTER TABLE, which gives the copy of the table the
   next value */
void ha_warp::update_create_info(HA_CREATE_INFO *create_info) {
  if(!(create_info->used_fields & HA_CREATE_USED_AUTO) &&
     table->found_next_number_field != nullptr) {
    create_info->auto_increment_value = autoinc_next_value();
  }
}

/* The values that the statement did not use are given back, if no other
   statement has taken values since */
void ha_warp::release_auto_increment() {
  if(autoinc_reserved_end != 0 && next_insert_id > 0) {
    mysql_mutex_lock(&share->mutex);
    if(share->autoinc_next == autoinc_reserved_end &&
       next_insert_id < autoinc_reserved_end) {
      share->autoinc_next = next_insert_id;
      share->autoinc_dirty = true;
    }
    mysql_mutex_unlock(&share->mutex);
    save_autoinc();
  }
  autoinc_reserved_end = 0;
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
    const uint64_t rows = warp_table_estimate(share->data_dir_name, where);
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
#include "ha_warp_part.h"

/* a partitioned table is a WARP table for each partition, see ha_warp_part */
static handler *warp_create_handler(handlerton *hton, TABLE_SHARE *table,
                                    bool partitioned, MEM_ROOT *mem_root) {
  if (partitioned) {
    ha_warp_part *file = new (mem_root) ha_warp_part(hton, table);
    if (file != nullptr && file->init_partitioning(mem_root)) {
      ::destroy_at(file);
      return nullptr;
    }
    return file;
  }
  return new (mem_root) ha_warp(hton, table);
}

static void warp_push_table_conditions(THD *thd, TABLE *table,
                                       AccessPath *filter, const JOIN *join) {
  // we can't do pushdown to non-WARP tables.
  ha_warp *ha = dynamic_cast<ha_warp *>(table->file);
  /* A partitioned table: the condition is made by its first partition (the
     partitions have the same columns) and given to all of them */
  ha_warp_part *const part_ha = dynamic_cast<ha_warp_part *>(table->file);
  if (ha == nullptr && part_ha != nullptr) ha = part_ha->condition_handler();
  if (ha == nullptr) return;

  /* The handler is reused by later statements.  A condition pushed down for
     an earlier statement whose scan did not finish (an error, for example)
     must not be applied to a statement that has no condition. */
  ha->push_where_clause = "";
  ha->outer_params.clear();
  ha->star_agg.reset();
  ha->par_join.reset();
  ha->par_agg.reset();
  ha->pj_follower.reset();
  if (part_ha != nullptr) part_ha->set_pushed_condition("");

  const Item *cond = (filter != nullptr) ? filter->filter().condition : nullptr;
  if (cond == nullptr && join->where_cond == nullptr) return;

  auto share = ha->get_warp_share();
  auto pushdown_info =
      get_or_create_pushdown_info(thd, ha->pd_alias().c_str(), share->data_dir_name);
  assert(pushdown_info != nullptr);

  ha->push_where_clause = "";
  const Item *remainder = nullptr;
  if (cond) {
    remainder = ha->cond_push(cond);
  }
  auto save_where = ha->push_where_clause;
  /* The WHERE clause is applied to the rows of the join.  A condition on the
     inner table of an outer join (WHERE d.k IS NULL, the usual anti join) is
     not a condition of the rows of the table: they are read with the ON clause
     only, a row without a match gets NULLs and then the condition decides. */
  const Table_ref *table_ref = table->pos_in_table_list;
  const bool inner_of_outer_join =
      table_ref != nullptr && table_ref->is_inner_table_of_outer_join();
  if (join->where_cond && !inner_of_outer_join) {
    ha->push_where_clause = "";
    ha->cond_push(join->where_cond);
  }
  /* either part may be empty: nothing of it was pushed */
  if (ha->push_where_clause != "" && save_where != "") {
    ha->push_where_clause += " AND ";
  }
  ha->push_where_clause += save_where;
  pushdown_info->filter = ha->push_where_clause;
  if (part_ha != nullptr) part_ha->set_pushed_condition(ha->push_where_clause);
  if (part_ha == nullptr) {
    /* the values of the query block around a subquery are known when the
       scan starts, see ha_warp::make_scan_where */
    ha->collect_outer_params(cond);
    ha->collect_outer_params(join->where_cond);
  }

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
                        (dynamic_cast<ha_warp *>(table->file) != nullptr ||
                         dynamic_cast<ha_warp_part *>(table->file) != nullptr)) {
                      found = true;
                    }
                    return found;
                  });
  return found;
}

/* Is the query block executed again and again, like a subquery that depends
   on the query block around it?  Its tables are scanned once for every
   execution, which the join pushdown does not support (it computes the join
   when the fact table is scanned first and leaves its state on the dimension
   tables, so the next execution reads them with the rows of the last one). */
static bool warp_block_is_executed_again(const JOIN *join) {
  for(const Query_block *block = join->query_block; block != nullptr;
      block = block->outer_query_block()) {
    const Query_expression *unit = block->master_query_expression();
    if(block->is_dependent() || (unit != nullptr && unit->uncacheable != 0)) {
      return true;
    }
  }
  return false;
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
#include "warp_par_join.h"
#include "warp_par_agg.h"

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
      warp_has_inner_nested_loop_table(root_path, join) ||
      warp_block_is_executed_again(join);
  WalkAccessPaths(root_path, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK,
                  func);
  warp_inner_nested_loop_table = false;
  /* the star joins that the engine can evaluate and aggregate itself */
  warp_try_star_aggregation(thd, root_path, join);
  warp_try_parallel_aggregation(thd, root_path, join);
  warp_try_parallel_join(thd, root_path, join);
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

/* The kind of the temporal columns that FastBit compares as numbers: 1 for
   TIME, 2 for DATE, DATETIME and TIMESTAMP, 0 for the other types. */
static int warp_temporal_kind(enum_field_types type) {
  if (warp_is_time_type(type)) return 1;
  switch (type) {
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
      return 2;
    default:
      return 0;
  }
}

/* Is the Item a value that does not change while the statement is executed
   (a literal, DATE_SUB('1997-01-01', INTERVAL 30 DAY),
   '1997-01-01' - INTERVAL 30 DAY, STR_TO_DATE(...), CURDATE(), ...) of the
   kind that MySQL compares with a temporal column of the given kind (see
   warp_temporal_kind)?  A TIME is not compared with a DATE or a DATETIME,
   and an expression with a subquery or a stored function is not evaluated
   while the condition is pushed down. */
static bool warp_is_temporal_const(Item *item, int kind) {
  if (kind == 0 || item->type() == Item::Type::FIELD_ITEM) return false;
  if (!item->const_for_execution() || item->has_subquery() ||
      item->has_stored_program()) {
    return false;
  }
  if (kind == 1) return warp_is_time_type(item->data_type());
  /* MySQL compares a DATE, DATETIME or TIMESTAMP column as a date with a
     value that is a date or time stamp or that is a string, which it
     converts to a date (DATE_SUB of a string, for example, is a string) */
  if (warp_is_time_type(item->data_type())) return false;
  return item->is_temporal_with_date() ||
         item->result_type() == STRING_RESULT;
}

/* Writes a string constant as a quoted FastBit string literal in the
   character set cs of the column it is compared with, the way MySQL
   converts the constant to compare it in the collation of the column.
   The constants compared with a binary string are its bytes, encoded (see
   warp_quote_string), a hexadecimal string (x'00ff') included.  Returns false
   if the constant is not a plain string that FastBit compares like MySQL: a
   constant that is not a string, a NULL, a string that can not be converted
   to the character set without a loss and a string with a NUL character
   compared with a string that is not binary (FastBit strings end at a
   NUL). */
static bool warp_quote_string_const(const Item *item, const CHARSET_INFO *cs,
                                    std::string &quoted) {
  const bool binary = cs == &my_charset_bin;
  if(item->type() != Item::Type::STRING_ITEM &&
     !(binary && item->type() == Item::Type::HEX_BIN_ITEM)) {
    return false;
  }
  Item *citem = const_cast<Item *>(item);
  String s;
  String *val = citem->val_str(&s);
  if(val == NULL) {
    return false;
  }
  String converted;
  if(!binary && !my_charset_same(val->charset(), cs)) {
    uint errors = 0;
    if(converted.copy(val->ptr(), val->length(), val->charset(), cs,
                      &errors) ||
       errors != 0) {
      return false;
    }
    val = &converted;
  }
  return warp_quote_string(val->ptr(), val->length(), binary, quoted);
}

/* Is a comparison of the string column fld evaluated by FastBit in the
   same way as by MySQL?  MySQL compares the column and the constants in the
   collation of the comparison, which has to be the collation of the column:
   FastBit compares the strings in that collation (see
   warp_collate_prefix).  With another collation (an explicit COLLATE, for
   example) MySQL has to do it. */
static bool warp_string_column_pushable(Item_func *func, const Field *fld) {
  const enum_field_types rt = fld->real_type();
  if(rt != MYSQL_TYPE_VARCHAR && rt != MYSQL_TYPE_STRING &&
     rt != MYSQL_TYPE_VAR_STRING) {
    return false;
  }
  if(!warp_string_column_supported(fld)) {
    return false;
  }
  const CHARSET_INFO *cmp_cs = func->compare_collation();
  return cmp_cs != NULL && cmp_cs == fld->charset();
}

/* Pushes a comparison of a string column with string constants (=, <, <=,
   >, >= and BETWEEN) down to FastBit.  The result must be the same as
   MySQL's: FastBit compares the strings in the collation of the column,
   utf8mb4_bin byte by byte with the trailing spaces not counting (PAD SPACE,
   ibis::util::padSpaceCompare) and the other collations with the comparison
   function of the server (warp_string_compare).  A pushed condition is not
   evaluated by MySQL again, so the conditions that can not be pushed are
   left to it: the comparisons in another collation (see
   warp_string_column_pushable), binary strings, constants that are not
   plain strings and strings with a NUL character.

   op is the operator of the comparison (not used for BETWEEN).  Returns true
   and appends the FastBit condition to out if the condition was pushed. */
static bool warp_push_string_compare(Item_func *func, Item **args,
                                     bool is_between, const std::string &op,
                                     std::string &out) {
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
      else if(op != " = ") return false;
    } else {
      return false;
    }
    nconsts = 1;
  }

  const Field *fld = col_item->field;
  if(!warp_string_column_pushable(func, fld)) {
    return false;
  }

  std::string values[2];
  for(uint i = 0; i < nconsts; ++i) {
    if(!warp_quote_string_const(consts[i], fld->charset(), values[i])) {
      return false;
    }
  }

  const std::string field_index = std::to_string(fld->field_index());
  std::string cond;
  if(fld->is_nullable()) {
    cond = "(n" + field_index + " = 0 AND ";
  }
  cond += warp_collate_prefix(fld->charset()) + "c" + field_index;
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

/* Can the equality of an ENUM column with a string constant be evaluated by
   FastBit?  FastBit compares the bytes of the strings, with the spaces at
   the end not counting, which is how MySQL compares utf8mb4_bin strings.
   MySQL compares the column and the constant in the collation of the
   comparison, which has to be utf8mb4_bin too: with another collation
   ('A' = 'a') MySQL has to do it.  Binary strings and strings with a NUL
   character are left to MySQL too.  The equality of the other string
   columns is pushed by warp_push_string_compare. */
static bool warp_string_eq_pushable(Item_func *func, Item **args) {
  if(func->arg_count != 2) {
    return false;
  }
  const Item_field *col_item = NULL;
  const Item *constant = NULL;
  if(args[0]->type() == Item::Type::FIELD_ITEM) {
    col_item = down_cast<Item_field *>(args[0]);
    constant = args[1];
  } else if(args[1]->type() == Item::Type::FIELD_ITEM) {
    col_item = down_cast<Item_field *>(args[1]);
    constant = args[0];
  } else {
    return false;
  }
  const Field *fld = col_item->field;
  if(fld->real_type() != MYSQL_TYPE_ENUM) {
    return false;
  }
  if(fld->charset()->number != 46) {
    return false;
  }
  const CHARSET_INFO *cmp_cs = func->compare_collation();
  if(cmp_cs == NULL || cmp_cs->number != 46) {
    return false;
  }
  std::string quoted;
  return warp_quote_string_const(constant, fld->charset(), quoted);
}

/* Pushes IN (...) of a string column with string constants down to FastBit.
   The same conditions as for the comparisons of warp_push_string_compare
   hold.  Returns true and appends the FastBit condition to out if the
   condition was pushed. */
static bool warp_push_string_in(Item_func *func, Item **args, std::string &out) {
  const uint nargs = func->arg_count;
  if(nargs < 2 || args[0]->type() != Item::Type::FIELD_ITEM) {
    return false;
  }
  const Field *fld = down_cast<Item_field *>(args[0])->field;
  if(!warp_string_column_pushable(func, fld)) {
    return false;
  }
  const std::string field_index = std::to_string(fld->field_index());
  std::string cond;
  if(fld->is_nullable()) {
    cond = "(n" + field_index + " = 0 AND ";
  }
  cond += warp_collate_prefix(fld->charset()) + "c" + field_index + " IN (";
  for(uint i = 1; i < nargs; ++i) {
    std::string quoted;
    if(!warp_quote_string_const(args[i], fld->charset(), quoted)) {
      return false;
    }
    if(i > 1) {
      cond += ", ";
    }
    cond += quoted;
  }
  cond += ")";
  if(fld->is_nullable()) {
    cond += ")";
  }
  out += cond;
  return true;
}

/* Pushes a comparison of a DECIMAL column with constants (=, <>, <, <=, >,
   >=, <=> and BETWEEN) down to FastBit.  WARP stores a DECIMAL as text,
   which FastBit compares by value with decimal_cmp.  MySQL compares a
   DECIMAL with an integer or a DECIMAL exactly, so only those constants are
   pushed: a constant that is a double (or a string) makes MySQL compare
   doubles.  Returns true and appends the FastBit condition to out if the
   condition was pushed. */
static bool warp_push_decimal(Item_func *func, Item **args, bool is_between,
                              const std::string &op, std::string &out) {
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
    } else {
      return false;
    }
    nconsts = 1;
  }

  if(!is_between && fb_op != " = " && fb_op != " != " && fb_op != " < " &&
     fb_op != " <= " && fb_op != " > " && fb_op != " >= ") {
    return false; /* IN lists, for example */
  }

  std::string values[2];
  for(uint i = 0; i < nconsts; ++i) {
    Item *citem = const_cast<Item *>(consts[i]);
    if(citem->type() == Item::Type::INT_ITEM) {
      if(!warp_format_int(citem, false, false, values[i])) {
        return false;
      }
    } else if(citem->type() == Item::Type::DECIMAL_ITEM) {
      String s;
      String *val = citem->val_str(&s);
      if(val == NULL) {
        return false;
      }
      values[i].assign(val->ptr(), val->length());
    } else {
      return false;
    }
    /* only the plain notation: sign, digits and a decimal point */
    const std::string &v = values[i];
    size_t digits = 0;
    for(size_t j = 0; j < v.length(); ++j) {
      const char c = v[j];
      if(c >= '0' && c <= '9') {
        ++digits;
      } else if(!((c == '-' && j == 0) || c == '.')) {
        return false;
      }
    }
    if(digits == 0) {
      return false;
    }
  }

  const std::string field_index = std::to_string(col_item->field->field_index());
  const bool nullable = col_item->field->is_nullable();
  const std::string col = "c" + field_index;
  std::string cond;
  if(nullable) {
    cond = "(n" + field_index + " = 0 AND ";
  }
  if(is_between) {
    cond += "decimal_cmp " + col + " BETWEEN '" + values[0] + "' AND '" +
            values[1] + "'";
  } else if(fb_op == " = ") {
    cond += "decimal_cmp " + col + " == '" + values[0] + "'";
  } else if(fb_op == " != ") {
    cond += "NOT (decimal_cmp " + col + " == '" + values[0] + "')";
  } else {
    cond += "decimal_cmp " + col + fb_op + "'" + values[0] + "'";
  }
  if(nullable) {
    cond += ")";
  }
  out += cond;
  return true;
}

/* The integer columns whose comparisons with a value of the query block around
   a subquery are given to the scan (see warp_outer_param) */
static bool warp_outer_param_column(const Field *field) {
  switch(field->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      return true;
    default:
      return false;
  }
}

/* Collects the conditions of cond that the scan can use with the values of the
   outer query block: column OP value, with the value made of columns of the
   query blocks around the subquery only.  Only the conditions that are joined
   with AND count.  Nothing is removed from the condition, MySQL still
   evaluates it for the rows that the scan returns. */
void ha_warp::collect_outer_params(const Item *cond) {
  if(cond == nullptr) return;
  if(cond->type() == Item::Type::COND_ITEM) {
    Item_cond *item_cond = down_cast<Item_cond *>(const_cast<Item *>(cond));
    if(item_cond->functype() != Item_func::Functype::COND_AND_FUNC) return;
    List_iterator_fast<Item> it(*item_cond->argument_list());
    for(Item *item = it++; item != nullptr; item = it++) {
      collect_outer_params(item);
    }
    return;
  }
  if(cond->type() != Item::Type::FUNC_ITEM) return;
  const Item_func *func = down_cast<const Item_func *>(cond);
  if(func->arg_count != 2) return;
  const char *op = nullptr;
  const char *flipped = nullptr;
  switch(func->functype()) {
    case Item_func::Functype::EQ_FUNC: op = " = ";  flipped = " = ";  break;
    case Item_func::Functype::LT_FUNC: op = " < ";  flipped = " > ";  break;
    case Item_func::Functype::LE_FUNC: op = " <= "; flipped = " >= "; break;
    case Item_func::Functype::GT_FUNC: op = " > ";  flipped = " < ";  break;
    case Item_func::Functype::GE_FUNC: op = " >= "; flipped = " <= "; break;
    default: return;
  }
  Item *a0 = func->arguments()[0];
  Item *a1 = func->arguments()[1];
  /* the column of this table is on either side; the value may be a column of
     the query block around the subquery (also an Item_field) */
  auto is_column_of_table = [this](Item *item) {
    if(item->type() != Item::Type::FIELD_ITEM) return false;
    const Field *f = down_cast<Item_field *>(item)->field;
    return f != nullptr && f->table == table;
  };
  Item *column = a0;
  Item *value = a1;
  if(!is_column_of_table(a0)) {
    column = a1;
    value = a0;
    op = flipped;
  }
  if(!is_column_of_table(column) || is_column_of_table(value)) return;
  const Field *field = down_cast<Item_field *>(column)->field;
  if(!warp_outer_param_column(field)) {
    return;
  }
  /* a value that only depends on the query blocks around the subquery: it
     does not change while the subquery is executed once */
  const table_map used = value->used_tables();
  if((used & OUTER_REF_TABLE_BIT) == 0 || (used & ~OUTER_REF_TABLE_BIT) != 0 ||
     value->result_type() != INT_RESULT || value->has_subquery() ||
     value->has_stored_program()) {
    return;
  }
  warp_outer_param param;
  param.field_index = field->field_index();
  param.nullable = field->is_nullable();
  param.col_unsigned = (field->all_flags() & UNSIGNED_FLAG) != 0;
  param.bigint = field->real_type() == MYSQL_TYPE_LONGLONG;
  param.op = op;
  param.value = value;
  outer_params.push_back(param);
}

/* The condition of the scan that is started: the pushed down condition and the
   parameters with the values that they have now.  A value that can not be
   written exactly is left out, the scan then returns more rows. */
void ha_warp::make_scan_where() {
  scan_where = push_where_clause;
  for(const warp_outer_param &param : outer_params) {
    std::string cond;
    param.value->val_int();
    if(param.value->null_value) {
      /* a comparison with NULL is not true for any row */
      cond = "1=0";
    } else {
      const bool exact = param.bigint && !warp_int_is_exact_double(param.value);
      std::string val;
      if(!warp_format_int(param.value, exact, param.col_unsigned, val)) {
        continue;
      }
      const std::string index = std::to_string(param.field_index);
      if(param.nullable) {
        cond = "(n" + index + " = 0 AND c" + index + param.op + val + ")";
      } else {
        cond = "c" + index + param.op + val;
      }
    }
    if(scan_where == "" || scan_where == "1=1") {
      scan_where = cond;
    } else {
      scan_where = "(" + scan_where + ") AND (" + cond + ")";
    }
  }
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

    /* The parameters of a prepared statement (WHERE id = ?) are replaced by
       the constants that they have for this execution, in a copy of the
       arguments: the condition is shared with the next execution, which
       has other values.  sysbench and most applications use prepared
       statements. */
    std::vector<Item *> condition_args(tmp->arguments(),
                                       tmp->arguments() + tmp->arg_count);
    for(Item *&condition_arg : condition_args) {
      if(condition_arg->type() == Item::Type::PARAM_ITEM) {
        Item *value = down_cast<Item_param *>(condition_arg)->clone_item();
        if(value != nullptr) {
          condition_arg = value;
        }
      }
    }
    Item **arg = condition_args.data();
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
        
        /* the alias of the table of the column (the name of the column, as
           it is printed, is not always qualified: the hidden column of a
           functional index is printed as its expression) */
        const Field *col = down_cast<Item_field *>(arg[arg_num])->field;
        if(col == nullptr || col->table == nullptr ||
           strcmp(table->alias, col->table->alias) != 0) {
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
      const std::string f0_key = warp_pd_key(f0->m_table_ref, f0->m_table_ref->alias);
      const std::string f1_key = warp_pd_key(f1->m_table_ref, f1->m_table_ref->alias);
      auto f0_info = get_pushdown_info(table->in_use, f0_key.c_str());
      auto f1_info = get_pushdown_info(table->in_use, f1_key.c_str());
      
      if(f0_info == NULL || f1_info == NULL) {
        return 0;
      }
      
      bool this_is_dim_table = true;
      if(f1_info->datadir != share->data_dir_name) {
        this_is_dim_table = false; 
      }
      const char* dim_field_mysql_name;
      const char* fact_field_mysql_name;
      std::string dim_alias;  // the key of the dimension table (warp_pd_key)
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
        dim_alias = f1_key;
      } else {
        fact_table = f1_info;
        fact_field_mysql_name = f1->field_name;
        //fact_alias = f1->table_name;
        
        dim_table = f0_info;
        dim_field_mysql_name = f0->field_name;
        dim_alias = f0_key;
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

    /* FastBit evaluates the conditions on the columns of the types that it
       stores as numbers, as dates or as strings of the same order as MySQL's.
       The other types are stored as text of some other kind (SET, JSON,
       the bytes of a TEXT or BLOB) or are not supported (GEOMETRY): MySQL
       compares them, only IS NULL and IS NOT NULL (not for GEOMETRY) are
       left to the null markers. */
    for (uint i = 0; i < tmp->arg_count; ++i) {
      if(arg[i]->type() != Item::Type::FIELD_ITEM) continue;
      const enum_field_types frt =
          down_cast<Item_field *>(arg[i])->field->real_type();
      if(frt == MYSQL_TYPE_GEOMETRY) return 0;
      if(is_is_null || is_isnot_null) continue;
      switch(frt) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_BIT:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_ENUM:
          break;
        default:
          return 0;
      }
    }

    /* An ENUM column is stored as a dictionary coded string.  MySQL compares
       it with a number by the position of the value in the list, so only
       the equality with a string, which warp_string_eq_pushable checks,
       is evaluated by FastBit. */
    if(!is_is_null && !is_isnot_null) {
      for (uint i = 0; i < tmp->arg_count; ++i) {
        if(arg[i]->type() == Item::Type::FIELD_ITEM &&
           down_cast<Item_field *>(arg[i])->field->real_type() ==
               MYSQL_TYPE_ENUM &&
           !(is_eq && tmp->arg_count == 2 &&
             (arg[0]->type() == Item::Type::STRING_ITEM ||
              arg[1]->type() == Item::Type::STRING_ITEM))) {
          return 0;
        }
      }
    }

    /* A DECIMAL column is stored as text: FastBit compares it by value with
       decimal_cmp.  The other comparisons of a DECIMAL column are left to
       MySQL */
    if(!is_is_null && !is_isnot_null && op != " LIKE ") {
      bool decimal_col = false;
      for (uint i = 0; i < tmp->arg_count; ++i) {
        if(arg[i]->type() == Item::Type::FIELD_ITEM) {
          const enum_field_types rt =
              down_cast<Item_field *>(arg[i])->field->real_type();
          if(rt == MYSQL_TYPE_DECIMAL || rt == MYSQL_TYPE_NEWDECIMAL) {
            decimal_col = true;
          }
        }
      }
      if(decimal_col) {
        std::string decimal_condition;
        if(warp_push_decimal(tmp, arg, is_between, op, decimal_condition)) {
          where_clause += decimal_condition;
          return 1;
        }
        return 0;
      }
    }

    /* Strings compared with =, <, <=, >, >= and BETWEEN */
    if(is_between || is_eq || op == " < " || op == " <= " || op == " > " ||
       op == " >= ") {
      std::string string_condition;
      if(warp_push_string_compare(tmp, arg, is_between, op,
                                  string_condition)) {
        where_clause += string_condition;
        return 1;
      }
    }

    /* Strings in an IN list */
    if(is_in) {
      std::string string_condition;
      if(warp_push_string_in(tmp, arg, string_condition)) {
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
    int temporal_kind = 0;
    for (uint i = 0; i < tmp->arg_count; ++i) {
      if(arg[i]->type() == Item::Type::FIELD_ITEM) {
        const enum_field_types frt =
            down_cast<Item_field *>(arg[i])->field->real_type();
        if(warp_is_timestamp_type(frt)) {
          timestamp_col = true;
        }
        temporal_kind = warp_temporal_kind(frt);
      }
    }

    /* A constant on the left of the comparison goes to the right, the
       condition is written as column OP constant */
    Item *swapped_args[2];
    if(!is_between && !is_in && tmp->arg_count == 2 &&
       arg[0]->type() != Item::Type::FIELD_ITEM &&
       arg[1]->type() == Item::Type::FIELD_ITEM && op != " LIKE ") {
      swapped_args[0] = arg[1];
      swapped_args[1] = arg[0];
      arg = swapped_args;
      if(op == " < ") op = " > ";
      else if(op == " <= ") op = " >= ";
      else if(op == " > ") op = " < ";
      else if(op == " >= ") op = " <= ";
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
        } else if(fld->real_type() == MYSQL_TYPE_BIT) {
          /* up to 64 bits, unsigned */
          bigint_col = true;
          col_unsigned = true;
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
      /* A constant temporal expression is evaluated here and compared as
         the packed value that is stored in the column */
      if(warp_is_temporal_const(*arg, temporal_kind)) {
        uint64_t t;
        if(timestamp_col) {
          if(!warp_item_timestamp_value(*arg, current_thd, t)) return 0;
        } else {
          t=warp_item_temporal_value(*arg);
        }
        /* a comparison with NULL is never true, MySQL evaluates it */
        if((*arg)->null_value) return 0;
        /* the packed DATETIME values are about 2^60, more than a double
           holds: an exact 64-bit unsigned constant, or the comparison finds
           nothing when a column is scanned instead of looked up in an index */
        build_where_clause += std::to_string(t);
        if(t > (1ULL << 53)) build_where_clause += "U";
        continue;
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
        /* A comparison with NULL, also NULL in an IN list or as a bound of
           BETWEEN, is not true for any row, and x <=> NULL is true for the
           rows where x is NULL: MySQL evaluates them.  FastBit would compare
           with the value that is stored for the NULL rows. */
        return 0;
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
        if(!is_eq || !warp_string_eq_pushable(tmp, condition_args.data())) {
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
  auto fact_pushdown_info=get_pushdown_info(table->in_use, pd_alias().c_str());
  
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
    auto dim_pushdown_info = get_pushdown_info(table->in_use, join_it->second.alias.c_str());
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
    last_trx_id = 0;
    while(dim_cursor->fetch() == 0) {
      
      ++rownum;   

      /* Only the rows that the transaction can see have a key.  The rows of
         an updated row are all in the table, and the key of the old version
         must not take the place of the new one. */
      uint64_t dim_rowid = 0, dim_trx_id = 0;
      dim_cursor->getColumnAsULong("r", dim_rowid);
      dim_cursor->getColumnAsULong("t", dim_trx_id);
      current_rowid = dim_rowid;
      if(!is_trx_visible_to_read(dim_trx_id) || !is_row_visible_to_read(dim_rowid)) {
        continue;
      }
  
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
    last_trx_id = 0;
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
int ha_warp::start_stmt(THD *thd, thr_lock_type) {
  /* a statement under LOCK TABLES, see external_lock */
  auto current_trx = warp_get_trx(warp_hton, thd);
  if(current_trx != NULL && current_trx->dirty && !current_trx->autocommit &&
     current_trx->savepoint_query_id != (uint64_t)thd->query_id) {
    current_trx->write_savepoint(thd->query_id);
  }
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
      /* the statement can be rolled back alone (see warp_trx::rollback),
         one savepoint for all the tables of the statement */
      if(!current_trx->autocommit &&
         current_trx->savepoint_query_id != (uint64_t)thd->query_id) {
        current_trx->write_savepoint(thd->query_id);
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
  /* the log is opened by the first change of the transaction (see
     open_log), a transaction that only reads has none */
  trx->autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN | OPTION_TABLE_LOCK);
  trx_mutex.unlock();
  return trx;
}

/* The log of the transaction: its inserts, its deletes and the savepoints
   of its statements.  It is read only by the transaction itself, when a
   statement is rolled back and when the deletes are applied at commit; the
   logs are removed when the server starts (remove_stale_trx_logs), they are
   never read after a crash.  So the log is not synced: what makes a commit
   durable are the delete bitmap and the commit bitmap. */
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
  open_log();
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

void warp_trx::write_savepoint(uint64_t query_id) {
  open_log();
  if(fwrite(&savepoint_marker, sizeof(savepoint_marker), 1, log) != 1 ||
     ferror(log) != 0) {
    sql_print_error("failed to write a savepoint into transaction log: %s",
                    log_filename.c_str());
    assert(false);
  }
  savepoint_query_id = query_id;
}

void warp_trx::write_delete_log_rowid(uint64_t rowid) {
  open_log();
  has_deletes = true;
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
/* Applies the deletes in the log of the transaction to the delete bitmap.
   The caller holds commit_mtx. */
void warp_trx::apply_log_deletes() {
  if(log == NULL) {
    return;
  }
  int sz = 0;
  uint64_t rowid = 0;
  char marker;
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
}

/* The transactions that commit at the same time share the syncs of the delete
   bitmap and of the commit bitmap.  The first one that finds no commit in
   progress (the leader) commits all the transactions that are waiting, the
   others sleep until the leader has made their commit durable.  A transaction
   that arrives while a group is committing is in the next group. */
static std::mutex warp_group_mtx;
static std::condition_variable warp_group_cv;
static std::vector<warp_trx *> warp_group_queue;
static bool warp_group_leader_active = false;

static void warp_commit_group(const std::vector<warp_trx *> &group) {
  commit_mtx.lock();
  for(warp_trx *trx : group) {
    if(warp_state->open_trx.find(trx->trx_id) == warp_state->open_trx.end()) {
      sql_print_error("Open transaction is not registered as open");
      assert(false);
    }
    trx->apply_log_deletes();
  }

  // commit the deletes of all the transactions, one sync for the group
  if(warp_state->delete_bitmap->commit() != 0) {
    sql_print_error("Failed to commit delete bitmap %s", warp_state->delete_bitmap->get_fname().c_str());
    assert(false);
  }

  // mark the transactions committed: set their bits in the commit bitmap and
  // make sure they are on disk
  for(warp_trx *trx : group) {
    if(warp_state->commit_bitmap->set_bit_direct(trx->trx_id, false) != 0) {
      sql_print_error("Failed to write to the commit bitmap");
      assert(false);
    }
  }
  if(warp_state->commit_bitmap->sync_direct() != 0) {
    sql_print_error("Failed to sync the commit bitmap");
    assert(false);
  }

  // the transactions are not open any more
  for(warp_trx *trx : group) {
    warp_state->open_trx.erase(trx->trx_id);
    trx->open_registered = false;
  }
  commit_mtx.unlock();
}

// used when a transaction commits
// not called when statements commit
void warp_trx::commit() {
  if(!dirty) {
    commit_mtx.lock();
    close_log();
    commit_mtx.unlock();
    return;
  }

  /* warp_sync_commit = OFF: a transaction that only inserted rows is
     committed at once, its bit is written to the commit bitmap when its rows
     are durable (warp_global_data::sync_round).  A crash of the system loses
     it whole: its rows are not committed. */
  if(wrote_relaxed && !has_deletes) {
    commit_mtx.lock();
    warp_state->add_pending_commit(trx_id);
    warp_state->open_trx.erase(trx_id);
    open_registered = false;
    close_log();
    commit_mtx.unlock();
    return;
  }

  /* The other commits are durable when they return.  The commits before
     them, and the rows of this transaction that were not synced, are made
     durable first: the transactions are durable in the order of their
     commits.  A transaction that deletes rows commits this way, the delete
     bitmap does not know the transactions of its bits. */
  if(warp_state->has_pending()) {
    warp_state->sync_round();
  }

  /* the deletes are read from the log by the commit (apply_log_deletes) */
  open_log();
  int sz = fwrite(&commit_marker, sizeof(commit_marker), 1, log);
  if(sz != 1) {
    sql_print_error("failed to write commit marker into transaction log");
  }
  fflush(log);

  std::unique_lock<std::mutex> lock(warp_group_mtx);
  commit_done = false;
  warp_group_queue.push_back(this);
  while(!commit_done) {
    if(!warp_group_leader_active) {
      warp_group_leader_active = true;
      std::vector<warp_trx *> group;
      group.swap(warp_group_queue);
      lock.unlock();
      warp_commit_group(group);
      lock.lock();
      for(warp_trx *trx : group) {
        trx->commit_done = true;
      }
      warp_group_leader_active = false;
      warp_group_cv.notify_all();
    } else {
      warp_group_cv.wait(lock);
    }
  }
  lock.unlock();

  commit_mtx.lock();
  close_log();
  commit_mtx.unlock();
}

// used when a transaction or statement rolls back
void warp_trx::rollback(bool all) {
  commit_mtx.lock();
  /* a transaction without a log has changed nothing yet */
  if(dirty && log != NULL) {
    if(warp_state->open_trx.find(trx_id) == warp_state->open_trx.end()) {
      sql_print_error("Open transaction is not registered as open");
      assert(false);
    }
    if(all != ROLLBACK_STATEMENT) {
      const int sz = fwrite(&rollback_marker, sizeof(rollback_marker), 1, log);
      if(sz != 1) {
        sql_print_error("failed to write rollback marker into transaction log");
      }
    }
    fflush(log);
    if(all == ROLLBACK_STATEMENT) {
      /* The statement is what the log has after the last savepoint (a
         statement of a transaction that is not autocommit writes one before
         it changes anything, see write_savepoint).  Its inserts are undone
         by deleting the rows, its deletes by removing their history locks:
         the deletes of the log are only applied when the transaction
         commits.  An autocommit statement is rolled back with its
         transaction, its rows are never committed. */
      fseek(log, 0, SEEK_SET);
      clearerr(log);
      long savepoint_at = -1;
      std::vector<std::pair<char, uint64_t>> statement_changes;
      char marker;
      while(fread(&marker, sizeof(marker), 1, log) == 1) {
        if(marker == savepoint_marker) {
          savepoint_at = ftell(log) - (long)sizeof(marker);
          statement_changes.clear();
          continue;
        }
        if(marker == insert_marker || marker == delete_marker) {
          uint64_t rowid = 0;
          if(fread(&rowid, sizeof(uint64_t), 1, log) != 1) {
            sql_print_error("could not read from transaction log %s",
                            log_filename.c_str());
            break;
          }
          statement_changes.emplace_back(marker, rowid);
        }
      }
      if(savepoint_at >= 0) {
        for(const auto &change : statement_changes) {
          if(change.first == insert_marker) {
            if(warp_state->delete_bitmap->set_bit(change.second) != 0) {
              sql_print_error("could not set bit in deleted bitmap");
              assert(false);
            }
          } else {
            warp_state->remove_history_lock(change.second, trx_id);
          }
        }
        if(warp_state->delete_bitmap->is_dirty()) {
          if(warp_state->delete_bitmap->commit() != 0) {
            sql_print_error("could not commit delete bitmap for rollback of statement");
            assert(false);
          }
        }
        /* the log ends where the statement began, the next statement writes
           a savepoint again */
        fflush(log);
        if(ftruncate(fileno(log), savepoint_at) != 0) {
          sql_print_error("could not truncate transaction log %s",
                          log_filename.c_str());
        }
        savepoint_query_id = 0;
      }
      fseek(log, 0, SEEK_END);
      clearerr(log);
    }
  }

  if(all != ROLLBACK_STATEMENT) {
    /* a rolled back transaction is not open any more and its bit in the
       commit bitmap is never set, so its rows are not visible */
    warp_state->open_trx.erase(trx_id);
    open_registered = false;
    close_log();
  }

  commit_mtx.unlock();
}


/* ---------------------------------------------------------------------
   warp_sync_commit = OFF

   The rows of the statements that change data are written to the column
   files without waiting for the disk (ibis::zfile::setSyncAppends): their
   frames are written and the headers of the files refer to them at once, a
   header also says which frames are durable.  A transaction that only
   inserted is committed at once, in memory (pending_commits, which
   is_trx_committed looks at).  Every warp_sync_interval milliseconds, and
   before a commit that is durable, sync_round makes the written partitions
   durable, then writes the bits of the pending commits to the commit bitmap
   and syncs it.  After a crash of the system the partitions are cut back to
   the rows that are on disk in every column (repair_tables): the rows of a
   transaction whose bit is not in the bitmap may be lost, the others are
   there.
   --------------------------------------------------------------------- */

bool warp_global_data::is_pending_commit(uint64_t trx_id) {
  std::lock_guard<std::mutex> guard(durable_mtx);
  return pending_commits.count(trx_id) != 0;
}

void warp_global_data::add_pending_partition(const std::string &part_dir) {
  std::lock_guard<std::mutex> guard(durable_mtx);
  pending_partitions.insert(part_dir);
}

void warp_global_data::add_pending_commit(uint64_t trx_id) {
  std::lock_guard<std::mutex> guard(durable_mtx);
  pending_commits.insert(trx_id);
  pending_commit_order.push_back(trx_id);
  pending_commit_count.store(pending_commits.size(), std::memory_order_release);
  /* the commit survives a crash of the server, see open_pending_log */
  if(pending_log_fd >= 0 &&
     ::write(pending_log_fd, &trx_id, sizeof(trx_id)) != (ssize_t)sizeof(trx_id)) {
    sql_print_error("WARP: could not write to %s", pending_commit_file.c_str());
  }
}

/* The log of the pending commits starts with the boot id of the system.
   When the server starts and the log is of the same boot, only the server
   crashed: the system has every write of the server, the rows of the
   pending commits are complete, so they are committed.  After a crash of
   the system the log is not used, the rows that were not durable are cut
   (repair_tables) and the pending commits are lost.  Called when the server
   starts, after the commit bitmap is opened. */
void warp_global_data::open_pending_log() {
  char id[64] = {0};
  FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
  if(f != NULL) {
    if(fgets(id, sizeof(id), f) == NULL) id[0] = 0;
    fclose(f);
  }
  boot_id = std::string(id);
  while(!boot_id.empty() && (boot_id.back() == '\n' || boot_id.back() == ' ')) {
    boot_id.pop_back();
  }
  boot_id.resize(36, ' ');
  FILE *old = fopen(pending_commit_file.c_str(), "rb");
  if(old != NULL) {
    char old_id[36];
    uint64_t committed = 0;
    if(fread(old_id, sizeof(old_id), 1, old) == 1 && !boot_id.empty() &&
       boot_id.compare(0, 36, old_id, 36) == 0) {
      uint64_t trx_id = 0;
      while(fread(&trx_id, sizeof(trx_id), 1, old) == 1) {
        if(trx_id != 0 && commit_bitmap->set_bit_direct(trx_id, false) == 0) {
          ++committed;
        }
      }
      if(committed > 0 && commit_bitmap->sync_direct() != 0) {
        sql_print_error("Failed to sync the commit bitmap");
        assert(false);
      }
    }
    fclose(old);
    if(committed > 0) {
      sql_print_information("WARP: %llu commits of warp_sync_commit = OFF were "
                            "made durable after a crash of the server",
                            (unsigned long long)committed);
    }
  }
  pending_log_fd = ::open(pending_commit_file.c_str(),
                          O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
  if(pending_log_fd < 0 ||
     ::write(pending_log_fd, boot_id.data(), 36) != 36) {
    sql_print_error("WARP: could not write %s", pending_commit_file.c_str());
  }
  if(pending_log_fd >= 0) {
    (void) fdatasync(pending_log_fd);
  }
}

/* the log has the commits that are still pending.  Called with durable_mtx
   held. */
void warp_global_data::rewrite_pending_log() {
  if(pending_log_fd < 0) return;
  if(ftruncate(pending_log_fd, 36) != 0 ||
     lseek(pending_log_fd, 36, SEEK_SET) != 36) {
    sql_print_error("WARP: could not write %s", pending_commit_file.c_str());
    return;
  }
  for(uint64_t trx_id : pending_commit_order) {
    if(::write(pending_log_fd, &trx_id, sizeof(trx_id)) != (ssize_t)sizeof(trx_id)) {
      sql_print_error("WARP: could not write %s", pending_commit_file.c_str());
      return;
    }
  }
}

bool warp_global_data::has_pending() {
  std::lock_guard<std::mutex> guard(durable_mtx);
  return !pending_commits.empty() || !pending_partitions.empty();
}

/* syncs a file or a directory */
static void warp_sync_path(const std::string &path, bool directory) {
  const int fd = open(path.c_str(), (directory ? O_RDONLY | O_DIRECTORY : O_RDONLY) | O_CLOEXEC);
  if(fd >= 0) {
    if(directory ? fsync(fd) != 0 : fdatasync(fd) != 0) {
      sql_print_error("WARP: could not sync %s: %s", path.c_str(), strerror(errno));
    }
    close(fd);
  }
}

void warp_global_data::sync_round() {
  std::lock_guard<std::mutex> round_guard(round_mtx);
  /* The commits are taken before the partitions: the rows of a transaction
     are written, and its partition is pending, before it commits. */
  std::vector<uint64_t> commits;
  std::set<std::string> partitions;
  {
    std::lock_guard<std::mutex> guard(durable_mtx);
    commits = pending_commit_order;
    partitions.swap(pending_partitions);
  }
  if(commits.empty() && partitions.empty()) {
    return;
  }
  /* the rows: every file of the partitions, the partitions and their
     tables (new files and the -part.txt that took the place of the old) */
  std::set<std::string> tables;
  for(const std::string &part_dir : partitions) {
    warp_partition_write_lock data_lock(part_dir, false);
    std::error_code ec;
    for(const auto &entry : std::filesystem::directory_iterator(part_dir, ec)) {
      std::error_code ec2;
      if(!entry.is_regular_file(ec2)) continue;
      const std::string file = entry.path().string();
      if(file.size() > 4 && file.compare(file.size() - 4, 4, ".tmp") == 0) continue;
      if(ibis::zfile::makeDurable(file.c_str()) < 0) {
        sql_print_error("WARP: could not make %s durable", file.c_str());
      }
    }
    warp_sync_path(part_dir, true);
    const size_t slash = part_dir.rfind('/');
    if(slash != std::string::npos) {
      tables.insert(part_dir.substr(0, slash));
    }
  }
  for(const std::string &table_dir : tables) {
    warp_sync_path(table_dir, true);
  }
  /* then the commits */
  if(!commits.empty()) {
    for(uint64_t trx_id : commits) {
      if(commit_bitmap->set_bit_direct(trx_id, false) != 0) {
        sql_print_error("Failed to write to the commit bitmap");
        assert(false);
      }
    }
    if(commit_bitmap->sync_direct() != 0) {
      sql_print_error("Failed to sync the commit bitmap");
      assert(false);
    }
    std::lock_guard<std::mutex> guard(durable_mtx);
    for(uint64_t trx_id : commits) {
      pending_commits.erase(trx_id);
    }
    pending_commit_order.erase(pending_commit_order.begin(),
                               pending_commit_order.begin() + commits.size());
    pending_commit_count.store(pending_commits.size(), std::memory_order_release);
    rewrite_pending_log();
  }
}

void warp_global_data::start_syncer() {
  syncer_stopping = false;
  syncer = std::thread([this]() {
    my_thread_init();
    std::unique_lock<std::mutex> lock(durable_mtx);
    while(!syncer_stopping) {
      syncer_cv.wait_for(lock, std::chrono::milliseconds(my_sync_interval));
      if(syncer_stopping) break;
      /* the interval was changed: the wait starts again with it */
      if(syncer_interval_changed) {
        syncer_interval_changed = false;
        continue;
      }
      if(pending_commits.empty() && pending_partitions.empty()) continue;
      lock.unlock();
      sync_round();
      lock.lock();
    }
    lock.unlock();
    my_thread_end();
  });
}

void warp_global_data::stop_syncer() {
  {
    std::lock_guard<std::mutex> guard(durable_mtx);
    syncer_stopping = true;
    syncer_cv.notify_all();
  }
  if(syncer.joinable()) {
    syncer.join();
  }
  /* what is pending is made durable before the shutdown */
  sync_round();
}

static void warp_update_sync_interval(THD *, SYS_VAR *, void *var_ptr,
                                      const void *save) {
  *static_cast<uint *>(var_ptr) = *static_cast<const uint *>(save);
  if(warp_state != NULL) {
    std::lock_guard<std::mutex> guard(warp_state->durable_mtx);
    warp_state->syncer_interval_changed = true;
    warp_state->syncer_cv.notify_all();
  }
}

static void warp_update_sync_commit(THD *, SYS_VAR *, void *var_ptr,
                                    const void *save) {
  *static_cast<bool *>(var_ptr) = *static_cast<const bool *>(save);
  /* the commits that are pending are durable when the setting is ON */
  if(*static_cast<const bool *>(save) && warp_state != NULL) {
    warp_state->sync_round();
  }
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
    sql_print_information("First time startup - initializing new WARP database.");
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

  /* the commits of warp_sync_commit = OFF after a crash of the server */
  open_pending_log();

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
  if(next_trx_id > trx_id_reserved) {
    trx_id_reserved = next_trx_id + 1023;
    write();
  }
  const uint64_t id = next_trx_id;
  mtx.unlock();
  return id;
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
  trx_id_reserved = next_trx_id;
  next_rowid = state_record->next_rowid;
  state_counter = state_record->state_counter;

  return state_record->version;
}

/* ---------------------------------------------------------------------
   Recovery after a crash of the system

   The rows of warp_sync_commit = OFF that were not synced may be lost, in
   some columns and not in others, and the -part.txt of a partition may say
   more rows than the columns have.  Every column file keeps the frames that
   are durable and the valid ones after them (ibis::zfile::recover); a
   partition is then cut back to the number of rows that all its columns
   have.  The rows that are cut were written by transactions whose commit is
   not in the commit bitmap: they were never durable.
   --------------------------------------------------------------------- */

namespace {
struct warp_part_column {
  std::string name;
  std::string type;
};
struct warp_part_meta {
  bool valid = false;
  uint64_t rows = 0;
  std::vector<warp_part_column> columns;
  std::vector<std::string> lines;
};
}

/* reads a -part.txt: its lines, the number of rows and the columns */
static warp_part_meta warp_read_part_meta(const std::string &file) {
  warp_part_meta meta;
  std::ifstream in(file);
  if(!in) return meta;
  std::string line;
  bool in_header = false, header_seen = false, in_column = false;
  warp_part_column column;
  auto value_of = [](const std::string &l) {
    const size_t eq = l.find('=');
    std::string v = eq == std::string::npos ? "" : l.substr(eq + 1);
    v.erase(0, v.find_first_not_of(" \t\"'"));
    while(!v.empty() && (v.back() == ' ' || v.back() == '"' || v.back() == '\'' ||
                         v.back() == '\r')) v.pop_back();
    return v;
  };
  while(std::getline(in, line)) {
    meta.lines.push_back(line);
    if(line.compare(0, 12, "BEGIN HEADER") == 0) { in_header = true; continue; }
    if(line.compare(0, 10, "END HEADER") == 0) { in_header = false; header_seen = true; continue; }
    if(in_header && line.compare(0, 14, "Number_of_rows") == 0) {
      meta.rows = strtoull(value_of(line).c_str(), NULL, 10);
    }
    if(line.compare(0, 12, "Begin Column") == 0) { in_column = true; column = warp_part_column(); continue; }
    if(line.compare(0, 10, "End Column") == 0) {
      if(in_column && !column.name.empty()) meta.columns.push_back(column);
      in_column = false;
      continue;
    }
    if(in_column && line.compare(0, 4, "name") == 0) column.name = value_of(line);
    if(in_column && line.compare(0, 9, "data_type") == 0) column.type = value_of(line);
  }
  meta.valid = header_seen && !meta.columns.empty();
  return meta;
}

/* bytes of a value of a fixed size type, 0 for the strings */
static uint64_t warp_type_size(const std::string &type) {
  if(type == "BYTE" || type == "UBYTE") return 1;
  if(type == "SHORT" || type == "USHORT") return 2;
  if(type == "INT" || type == "UINT" || type == "FLOAT") return 4;
  if(type == "LONG" || type == "ULONG" || type == "DOUBLE" || type == "OID") return 8;
  return 0;
}

/* the logical content of a column file */
static bool warp_read_column(const std::string &file, uint64_t size,
                             std::vector<char> &content) {
  content.resize(size);
  return size == 0 ||
         ibis::zfile::readRange(file.c_str(), 0, size, content.data()) == (int64_t)size;
}

/* Repairs a partition after a crash, see above.  table_meta is the
   -part.txt of the table, which has the same columns. */
static void warp_repair_partition(const std::string &part_dir,
                                  const warp_part_meta &table_meta) {
  const std::string meta_file = part_dir + "/-part.txt";
  (void) unlink((meta_file + ".tmp").c_str());
  warp_part_meta meta = warp_read_part_meta(meta_file);
  const bool meta_found = meta.valid;
  if(!meta_found) {
    meta = table_meta;
    meta.rows = UINT64_MAX;
  }
  if(!meta.valid) return;
  /* the rows of every column, a file that recover changed (or a plain file)
     makes the partition suspect: then the strings are counted too */
  struct column_state {
    std::string file;
    uint64_t fixed = 0;
    uint64_t size = 0;
    uint64_t rows = 0;
  };
  std::vector<column_state> columns;
  bool suspect = !meta_found;
  bool any_file = false;
  for(const auto &column : meta.columns) {
    column_state state;
    state.file = part_dir + "/" + column.name;
    state.fixed = warp_type_size(column.type);
    const int64_t before = ibis::zfile::logicalSize(state.file.c_str());
    const int64_t after = before < 0 ? -1 : ibis::zfile::recover(state.file.c_str());
    if(before >= 0) any_file = true;
    if(after < 0) {
      /* no file, or one that can not be read: no row of it is there */
      state.size = 0;
      suspect = true;
    } else {
      state.size = (uint64_t)after;
      if(after != before || !ibis::zfile::isCompressed(state.file.c_str())) suspect = true;
    }
    if(state.fixed > 0) {
      state.rows = state.size / state.fixed;
      if(state.rows != meta.rows) suspect = true;
    }
    columns.push_back(state);
  }
  if(!any_file) {
    /* a partition that was created and has no data: nothing to keep */
    std::error_code ec;
    std::filesystem::remove_all(part_dir, ec);
    sql_print_information("WARP: recovery removed the empty partition %s", part_dir.c_str());
    return;
  }
  if(!suspect) return;
  std::vector<char> content;
  uint64_t rows = UINT64_MAX;
  for(auto &state : columns) {
    if(state.fixed == 0) {
      /* the strings end with a NUL */
      if(!warp_read_column(state.file, state.size, content)) {
        state.rows = 0;
      } else {
        state.rows = std::count(content.begin(), content.end(), '\0');
      }
    }
    rows = std::min(rows, state.rows);
  }
  if(rows == meta.rows) {
    bool same = true;
    for(const auto &state : columns) {
      if(state.rows != rows) same = false;
    }
    if(same) return;
  }
  sql_print_warning("WARP: recovery cuts the partition %s back to %llu rows "
                    "(its metadata had %llu)", part_dir.c_str(),
                    (unsigned long long)rows,
                    (unsigned long long)(meta.rows == UINT64_MAX ? 0 : meta.rows));
  if(rows == 0) {
    std::error_code ec;
    std::filesystem::remove_all(part_dir, ec);
    return;
  }
  /* the columns are cut to the rows */
  for(auto &state : columns) {
    if(state.rows == rows) continue;
    uint64_t bytes = rows * state.fixed;
    if(state.fixed == 0) {
      if(!warp_read_column(state.file, state.size, content)) continue;
      uint64_t seen = 0;
      bytes = 0;
      for(size_t i = 0; i < content.size(); ++i) {
        if(content[i] == 0 && ++seen == rows) {
          bytes = i + 1;
          break;
        }
      }
    }
    if(ibis::zfile::truncateLogical(state.file.c_str(), bytes) != 0) {
      sql_print_error("WARP: recovery could not cut %s to %llu bytes",
                      state.file.c_str(), (unsigned long long)bytes);
    }
  }
  /* the files made from the columns (positions of strings, indexes) are
     made again when they are needed */
  std::set<std::string> keep;
  for(const auto &state : columns) keep.insert(state.file);
  keep.insert(meta_file);
  std::error_code ec;
  for(const auto &entry : std::filesystem::directory_iterator(part_dir, ec)) {
    const std::string file = entry.path().string();
    if(keep.count(file) == 0) {
      std::error_code ec2;
      std::filesystem::remove(entry.path(), ec2);
    }
  }
  /* the metadata says the number of rows */
  const std::string tmp = meta_file + ".tmp";
  {
    std::ofstream out(tmp);
    const std::string name = part_dir.substr(part_dir.rfind('/') + 1);
    for(const std::string &line : meta.lines) {
      if(line.compare(0, 14, "Number_of_rows") == 0) {
        out << "Number_of_rows = " << rows << "\n";
      } else if(!meta_found && line.compare(0, 4, "Name") == 0) {
        out << "Name = " << name << "\n";
      } else {
        out << line << "\n";
      }
    }
  }
  warp_sync_path(tmp, false);
  if(rename(tmp.c_str(), meta_file.c_str()) != 0) {
    sql_print_error("WARP: recovery could not write %s", meta_file.c_str());
  }
  warp_sync_path(part_dir, true);
}

bool warp_global_data::repair_tables() {
  sql_print_information("WARP: the server did not shut down cleanly, checking the partitions");
  for(const std::string &table_dir : warp_list_tables()) {
    const warp_part_meta table_meta = warp_read_part_meta(table_dir + "/-part.txt");
    std::error_code ec;
    for(const auto &entry : std::filesystem::directory_iterator(table_dir, ec)) {
      std::error_code ec2;
      if(!entry.is_directory(ec2)) continue;
      const std::string name = entry.path().filename().string();
      if(name.size() < 2 || name[0] != 'p' ||
         name.find_first_not_of("0123456789", 1) != std::string::npos) {
        continue;
      }
      warp_repair_partition(entry.path().string(), table_meta);
    }
  }
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
  record.next_trx_id = trx_id_reserved > next_trx_id ? trx_id_reserved : next_trx_id;
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
  record.next_trx_id = trx_id_reserved > next_trx_id ? trx_id_reserved : next_trx_id;
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

void warp_global_data::remove_history_lock(uint64_t rowid, uint64_t trx_id) {
  std::unique_lock<std::shared_mutex> guard(history_lock_mtx);
  auto it = history_locks.find(rowid);
  if(it != history_locks.end() && it->second == trx_id) {
    history_locks.erase(it);
    history_lock_count.store(history_locks.size(), std::memory_order_release);
  }
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
  std::string extra;
  if (pushed_cond != nullptr) {
    extra = ", with pushed condition: " + ItemToString(pushed_cond);
  }
  /* the engine does the work of the plan above this scan */
  if (par_agg) {
    extra += ", with parallel aggregation";
  } else if (par_join) {
    extra += ", with parallel hash join";
  } else if (pj_follower) {
    extra += ", joined by the parallel hash join";
  }
  return extra;
}

std::string warp_pd_key(const Table_ref *ref, const char *alias) {
  if(ref == nullptr || ref->query_block == nullptr) {
    return std::string(alias);
  }
  return std::string(alias) + "@" + std::to_string(ref->query_block->select_number);
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

/* ha_warp.h defines the state of the engine, the partitioned tables are in
   this translation unit too */
#include "ha_warp_part.cc"
