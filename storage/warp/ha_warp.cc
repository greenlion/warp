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
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "storage/warp/ha_warp.h"
/* Stuff for shares */
mysql_mutex_t warp_mutex;
static std::unique_ptr<collation_unordered_multimap<std::string, WARP_SHARE *>>
    warp_open_tables;

static handler *warp_create_handler(handlerton *hton, TABLE_SHARE *table,
                                    bool partitioned, MEM_ROOT *mem_root);

static handler *warp_create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                                    MEM_ROOT *mem_root) {
  return new (mem_root) ha_warp(hton, table);
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
  NULL,             /* status variables                */
  system_variables, /* system variables    */
  NULL,             /* config options                  */
  0,                /* flags                           */
} mysql_declare_plugin_end;

 
static int warp_init_func(void *p) {
  DBUG_ENTER("warp_init_func");
  sql_print_error("WARP storage engine initialization started");
  handlerton *warp_hton;
  ibis::fileManager::adjustCacheSize(my_cache_size);
  ibis::init(NULL, "/tmp/fastbit.log");
  ibis::util::setVerboseLevel(0);
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
  
  // starts the database and reads in the database state, upgrades
  // tables and does crash recovery
  warp_state = new warp_global_data();
  
  assert(warp_state != NULL);
  sql_print_error("WARP storage engine initialization completed");
  DBUG_RETURN(0);
}

static int warp_done_func(void *) {
  sql_print_error("WARP storage engine shutdown started");
  warp_open_tables.reset();

  // destroying warp_state writes the state to disk
  delete warp_state;
  mysql_mutex_destroy(&warp_mutex);
  sql_print_error("WARP storage engine shutdown completed");
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
      blobroot(warp_key_memory_blobroot, BLOB_MEMROOT_ALLOC_SIZE) {
        warp_hton = hton;
      }

const char **ha_warp::bas_ext() const {  
  return ha_warp_exts;
}

int ha_warp::rename_table(const char * from, const char * to, const dd::Table* , dd::Table* ) {
  DBUG_ENTER("ha_example::rename_table ");
  std::string cmd = "mv " + std::string(from) + ".data/ " + std::string(to) + ".data/";
  
  system(cmd.c_str()); 
  DBUG_RETURN(0);
}

bool ha_warp::is_deleted(uint64_t rownum) {
  return warp_state->delete_bitmap->is_set(rownum);
}

void ha_warp::get_auto_increment(ulonglong, ulonglong, ulonglong,
                                 ulonglong *first_value,
                                 ulonglong *nb_reserved_values) {
  *first_value = stats.auto_increment_value ? stats.auto_increment_value : 1;
  *nb_reserved_values = ULLONG_MAX;
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
        attribute.append((*field)->data_ptr()[0]);

        no_quote = true;
        break;

      case MYSQL_TYPE_DATE:
        attribute.append(std::to_string((*field)->val_int()).c_str());
        no_quote = true;
        break;

      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIME2: {
        attribute.append(std::to_string((*field)->val_int()).c_str());
        no_quote = true;
      } break;

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

      for (; ptr < end_ptr; ptr++) {
        if(*ptr == '"') {
          buffer.append('\\');
          buffer.append('"');
        } else if(*ptr == '\r') {
          buffer.append('\\');
          buffer.append('r');
        } else if(*ptr == '\\') {
          buffer.append('\\');
          buffer.append('\\');
        } else if(*ptr == '\n') {
          buffer.append('\\');
          buffer.append('n');
        } else
          buffer.append(*ptr);
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
  get_or_create_trx(current_thd, current_trx);
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

/*
  Populates the comma separarted list of all the columns that need to be read
  from the storage engine for this query.
*/
int ha_warp::set_column_set() {
  // bool read_all;
  DBUG_ENTER("ha_warp::set_column_set");
  column_set = "";

  /* We must read all columns in case a table is opened for update */
  // read_all = !bitmap_is_clear_all(table->write_set);
  int count = 0;
  for (Field **field = table->field; *field; field++) {
    if(bitmap_is_set(table->read_set, (*field)->field_index())) {
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

  DBUG_PRINT("ha_warp::set_column_set", ("column_list=%s", column_set.c_str()));

  DBUG_RETURN(count + 1);
}

/*
  Populates the comma separarted list of all the columns that need to be read
  from the storage engine for this query for a given index.
  FIXME:
  I don't think this version of the function ever needs to be used and may be
  eliminated in the future...
*/
int ha_warp::set_column_set(uint32_t idxno) {
  DBUG_ENTER("ha_warp::set_column_set");
  index_column_set = "";
  uint32_t count = 0;

  for (uint32_t i = 0; i < table->key_info[idxno].actual_key_parts; ++i) {
    uint16_t fieldno = table->key_info[idxno].key_part[i].field->field_index();
    bool may_be_null = table->key_info[idxno].key_part[i].field->is_nullable();
    ++count;

    /* this column must be read from disk */
    index_column_set += std::string("c") + std::to_string(fieldno);

    /* Add the NULL bitmap for the column if the column is NULLable */
    if(may_be_null) {
      index_column_set += "," + std::string("n") + std::to_string(fieldno);
    }
    index_column_set += ",";
  }

  index_column_set = index_column_set.substr(0, index_column_set.length() - 1);

  DBUG_PRINT("ha_warp::set_column_set",
             ("column_list=%s", index_column_set.c_str()));

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
  // bool read_all = !bitmap_is_clear_all(table->write_set);

  /* First step: gather stats about the table and the resultset.
     These are needed to size the NULL bitmap for the row and to
     properly format that bitmap.  DYNAMIC rows resultsets (those
     that contain variable length fields) reserve an extra bit in
     the NULL bitmap...
  */
  for (Field **field = table->field; *field; field++) {
    buffer.length(0);
    if(bitmap_is_set(table->read_set, (*field)->field_index())) {
      // DBUG_PRINT("ha_warp::find_current_row", ("Getting value for field:
      // %s",(*field)->field_name));
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
            unsigned char tmp = 0;
            rc = cursor->getColumnAsUByte(cname.c_str(), tmp);
            rc = (*field)->store(tmp, true);
          } else {
            char tmp = 0;
            rc = cursor->getColumnAsByte(cname.c_str(), tmp);
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
            // size_t length = blob_field->get_length(blob_field->ptr);
            size_t length = blob_field->get_length();
            // BLOB data is not stored inside buffer. It only contains a
            // pointer to it. Copy the BLOB data into a separate memory
            // area so that it is not overwritten by subsequent calls to
            // Field::store() after moving the offset.
            if(length > 0) {
              const unsigned char *old_blob;
              // blob_field->get_ptr(&old_blob);
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
          rc = (*field)->store(tmp, true);
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
  if(base_table) {
    delete base_table;
  }

  base_table = new ibis::mensa(share->data_dir_name);
  stats.records = base_table->nRows();

  delete base_table;
  base_table = NULL;
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

  // auto get_status = [](void*, int concurrent_insert) {
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

  if(writer) delete writer;
  writer = NULL;

  if(cursor) delete cursor;
  cursor = NULL;

  if(filtered_table) delete filtered_table;
  filtered_table = NULL;

  if(base_table) delete base_table;
  base_table = NULL;

  DBUG_RETURN(free_share(share));
}

int ha_warp::get_writer_partno(ibis::tablex* writer, char* datadir) {
  ibis::partList parts;
  int partition_count = ibis::util::gatherParts(parts, datadir);

  /* The fastbit table name has to be the same as the partition number
     which is the directory name of the partition
  */
  ibis::part *found = NULL;
  int partno = 0;
  if(partition_count > 1) {
    /* skip the first partition, which is just for metadata and is always empty
     */
    for (auto it = parts.begin() + 1; it < parts.end(); ++it, ++partno) {
      auto part = *it;
      if(part->nRows() + writer->mRows() <= my_partition_max_rows) {
        found = part;
        break;
      }
    }
  }
  /* If the table is empty, then partno will be zero and found will be NULL.
     Otherwise, if found != NULL and partno <= partition_count then a partition
     is fouund that will fit all the new rows.
  */
  if(found == NULL) {
    partno = partition_count - 1;
  }
  return partno;
}
/* write the rows in the background and destroy the writer*/
void ha_warp::background_write(ibis::tablex *writer, char *datadir,
                               TABLE *table, 
                               WARP_SHARE *share) {
  
  mysql_mutex_lock(&share->mutex);
  auto partno = ha_warp::get_writer_partno(writer, datadir);
  std::string part_dir = "";
  std::string part_name = "";
  part_dir = std::string(datadir) + "/p" + std::to_string(partno);
  part_name = "p" + std::to_string(partno);

  writer->write(part_dir.c_str(), part_name.c_str());
  writer->clearData();

  delete writer;

  /* rebuild any indexed columns */
  maintain_indexes(datadir, table);
  mysql_mutex_unlock(&share->mutex);
};

/* write the rows in the background and destroy the writer*/
void ha_warp::foreground_write() {
  auto partno = ha_warp::get_writer_partno(writer, share->data_dir_name);
  std::string part_dir = "";
  std::string part_name = "";
  part_dir = std::string(share->data_dir_name) + "/p" + std::to_string(partno);
  part_name = "p" + std::to_string(partno);

  writer->write(part_dir.c_str(), part_name.c_str());
  writer->clearData();
  delete writer;
  writer = NULL;
  maintain_indexes(share->data_dir_name, table);
};

std::string ha_warp::make_unique_check_clause() {
  std::string unique_check_where_clause = "";
  for (uint i = 0; i < table_share->keys; ++i) {
    bool needs_quote = false;
    bool convert_from_int = false;
    uint tmpint = 0;
    String key_part_tmp;
    String key_part_esc;
    // reserve space for max_key_part_length with a 4 byte character set
    //  with space for escaping values
    key_part_tmp.reserve((max_key_part_length(NULL) * 4) + 1);
    key_part_esc.reserve((max_key_part_length(NULL) * 4 * 2) + 1);

    if(table->key_info[i].flags & HA_NOSAME) {
      KEY *ki = table->key_info + i;
      if(unique_check_where_clause.length() > 0) {
        unique_check_where_clause += " OR (";
      } else {
        unique_check_where_clause = "(";
      }
      
      for (uint n = 0; n < ki->actual_key_parts; n++) {
        if(n > 0) {
          unique_check_where_clause += " AND ";
        }
        Field *f = ki->key_part[n].field;
        unique_check_where_clause +=
            "c" + std::to_string(ki->key_part[n].fieldnr-1) + " = ";

        switch(f->real_type()) {
          case MYSQL_TYPE_VAR_STRING:
          case MYSQL_TYPE_VARCHAR:
          case MYSQL_TYPE_STRING:
          case MYSQL_TYPE_TINY_BLOB:
          case MYSQL_TYPE_MEDIUM_BLOB:
          case MYSQL_TYPE_LONG_BLOB:
          case MYSQL_TYPE_BLOB:
          case MYSQL_TYPE_JSON:
            needs_quote = true;
            break;
          case MYSQL_TYPE_DATE:
          case MYSQL_TYPE_TIME:
          case MYSQL_TYPE_TIMESTAMP:
          case MYSQL_TYPE_DATETIME:
          case MYSQL_TYPE_YEAR:
          case MYSQL_TYPE_TIMESTAMP2:
          case MYSQL_TYPE_DATETIME2:
            needs_quote = false;
            convert_from_int = true;
            break;
          default:
            needs_quote = false;
        }

        if(needs_quote) {
          ki->key_part[n].field->val_str(&key_part_tmp);
          escape_string_for_mysql(f->charset(), key_part_esc.c_ptr(),
                                  max_supported_key_part_length(NULL),
                                  key_part_tmp.c_ptr(), key_part_tmp.length());
        } else {
          memset(key_part_tmp.c_ptr(), 0,
                 max_supported_key_part_length(NULL) * 4);
          if(convert_from_int) {
            tmpint = ki->key_part[n].field->val_int();
          } else {
            ki->key_part[n].field->val_str(&key_part_esc);
          }
        }
      
        if(!convert_from_int) {
          unique_check_where_clause +=
              std::string(key_part_esc.c_ptr(), key_part_esc.length());
        } else {
          unique_check_where_clause += std::to_string(tmpint);
        }
        unique_check_where_clause += ")";
      }
    }
  }
  return unique_check_where_clause;
}
/*
  This is an INSERT.  The row data is converted to CSV (just like the CSV
  engine) and this is passed to the storage layer for processing.  It would be
  more efficient to construct a vector of rows to insert (to also support bulk
  insert).
*/
int ha_warp::write_row(uchar *buf) {
  DBUG_ENTER("ha_warp::write_row");
  ha_statistic_increment(&System_status_var::ha_write_count);
  mysql_mutex_lock(&share->mutex);
  std::string insert_log_filename = "";
  if(share->next_rowid == 0) {
    share->next_rowid = warp_state->get_next_rowid_batch();
  } else {
    current_rowid = share->next_rowid--;
  }
  mysql_mutex_unlock(&share->mutex);
  get_or_create_trx(current_thd, current_trx);
  assert(current_trx != NULL);
  
  current_trx->open_insert_log();

  /* check for duplicate key values */
  int errcode = 0;
  current_trx = NULL;
  last_trx_id = 0;
  uint64_t row_trx_id;

  // this function uses the table->field object to access the data
  // in the row buffer!
  std::string unique_check_where_clause = make_unique_check_clause();
  
  if(unique_check_where_clause != "") {
    auto tbl = new ibis::mensa(share->data_dir_name); // base table
    if(!tbl) {
      errcode = HA_ERR_INDEX_CORRUPT;
    } else {  
      auto rows = // found table (select returns a table with the results)
          tbl->select("r,t", unique_check_where_clause.c_str());
      
      if(rows) {
        
        auto csr = rows->createCursor();
        uint64_t rowid=0;
        int fetch_return_code = -1;
        // if the next trx_id is the same as the last trx_id
        // then the visibility check does not have to be done
        // again
        last_trx_id = 0;
        is_visible = false;
        
        
        if(csr) {
          for(uint64_t i=0;errcode == 0 && i<rows->nRows();++i) {
            csr->fetch();
            fetch_return_code  = csr->getColumnAsULong("r", rowid);

            if(fetch_return_code < 0) {
              errcode = HA_ERR_INDEX_CORRUPT;
              break;
            }

            fetch_return_code  = csr->getColumnAsULong("t", row_trx_id);
            
            // If this is our transaction, then the row is of course visible
            // If the row_trx_id is zero then this table came from
            // beta 1 and it is visible.  
            // If this is the same transaction
            // as the last transaction that was checked for visibility it
            // does not have to be checked again, which avoid the expensive
            // checking of the bitmap or iteration of open transctions.
            if(last_trx_id != row_trx_id) {
              last_trx_id = row_trx_id;
              if(row_trx_id == current_trx->trx_id || row_trx_id == 0) {
                is_visible = true;
              } else {  
                // since this is not our transaction, first check the commit
                // bitmap.  If it was commited by another transaction then it
                // is visible to the unique check
                if(warp_state->commit_bitmap->is_set(row_trx_id)) {
                  is_visible = true;
                } else {
                  // UNIQUE CHECKS MUST BE READ UNCOMMITED!
                  // if the row was not committed, it is still visible if it 
                  // was writen by another open transaction that has not yet
                  // committed, so check the list of open transaction ids
                  // and if the transaction is open, the row is visible to
                  // the unique check
                  is_visible = warp_state->is_transaction_open(row_trx_id);
                } 
              }  
            }
            
            if(!is_visible) {
              continue;
            }
            if(fetch_return_code < 0) {
              errcode = HA_ERR_INDEX_CORRUPT;
              break;
            }

            if(!warp_state->delete_bitmap->is_set(rowid)) {
                errcode = HA_ERR_FOUND_DUPP_KEY;
            }  
          }
          delete csr;
        }
        delete rows;
      } 
      delete tbl;
    }
     DBUG_RETURN(errcode);   
  }
  /* This will return a cached writer unless a background
    write was started on the last insert.  In that case
    a new writer is constructed because the old one will
    still be background writing.
  */
  create_writer(table);
  mysql_mutex_lock(&share->mutex);
  current_rowid = ++share->next_rowid;
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
  DBUG_PRINT("ha_warp::write_row", ("row_data=%s", buffer.c_ptr()));

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
  if(unique_check_where_clause != "") {
    foreground_write();
  } else if(writer->mRows() >= my_write_cache_size) {
    /* spawn the background writer if the cache size is exceeded. */
    ibis::tablex *writing_writer = writer;
    writer = NULL;
    /* Spawn a background thread using a static method ha_warp::background_write
    */
    std::thread writer_thread(ha_warp::background_write, writing_writer,
                              share->data_dir_name, table, share);

    /* Don't worry about reaping the thread.  It will complete work and then exit.
    */
    writer_thread.detach();
    // writer->write(share->data_dir_name,"_");
    // writer->clearData();
  }
  DBUG_RETURN(0);
}

int ha_warp::update_row(const uchar *, uchar *new_data) {
  DBUG_ENTER("ha_warp::update_row");
  uint64_t deleted_rowid = current_rowid;
  auto retval = write_row(new_data);
  if(retval == 0) {
    warp_state->delete_bitmap->set_bit(deleted_rowid);
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
  warp_state->delete_bitmap->set_bit(current_rowid);
  ha_statistic_increment(&System_status_var::ha_delete_count);
  stats.records--;
  DBUG_RETURN(0);
}

int ha_warp::delete_table(const char *table_name, const dd::Table *) {
  DBUG_ENTER("ha_warp::delete_table");
  // my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "DROP is not supported)");
  // DBUG_RETURN(HA_ERR_UNSUPPORTED);

  // FIXME: this needs to be safer
  // system((std::string("rm -rf ") +
  // std::string(share->data_dir_name)).c_str());
  std::string cmdline =
      std::string("rm -rf ") + std::string(table_name) + ".data/";
  int rc = system(cmdline.c_str());
  ha_statistic_increment(&System_status_var::ha_delete_count);
  DBUG_RETURN(rc != 0);
}

// int ha_warp::truncate(dd::Table *dd) {
int ha_warp::delete_all_rows() {
  int rc = delete_table(share->table_name.c_str(), NULL);
  if(rc) return rc;
  return create(share->table_name.c_str(), table, NULL, NULL);
}

/*
  ::info() is used to return information to the optimizer.
  Currently this table handler doesn't implement most of the fields
  really needed. SHOW also makes use of this data
*/
int ha_warp::info(uint) {
  DBUG_ENTER("ha_warp::info");
  // if(!records_is_known && stats.records < 2) stats.records = 2;

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

  DBUG_RETURN(0);
}

/* Fastbit will normally maintain the indexes automatically, but if the type
   of bitmap index is to be set manually, the comment on the field will be
   taken into account. */
void ha_warp::maintain_indexes(char *datadir, TABLE *table) {
  ibis::mensa::table *base_table = ibis::mensa::create(datadir);

  for (uint16_t idx = 0; idx < table->s->keys; ++idx) {
    std::string cname =
        "c" +
        std::to_string(table->key_info[idx].key_part[0].field->field_index());

    // a field can have an index= comment that indicates an index spec.
    // See here: https://sdm.lbl.gov/fastbit/doc/indexSpec.html
    std::string comment(table->key_info[idx].key_part[0].field->comment.str,
                        table->key_info[idx].key_part[0].field->comment.length);
    if(comment != "" && comment.substr(0, 6) != "index=") {
      comment = "";
    }
    base_table->buildIndex(cname.c_str(), comment.c_str());
  }
  delete (base_table);
  return;
}

/* The ::extra function is called a bunch of times before and after various
   storage engine operations. I think it is used as a hint for faster alter
   for example. Right now, in warp if there are any dirty rows buffered in
   the writer object, flush them to disk when ::extra is called.   Seems to
   work.
*/
int ha_warp::extra(enum ha_extra_function) {
  if(writer != NULL && writer->mRows() > 0) {
    /* foreground write actually... */
    background_write(writer, share->data_dir_name, table, share);
    writer = NULL;
  }
  return 0;
}

int ha_warp::repair(THD *, HA_CHECK_OPT *) {
  DBUG_ENTER("ha_warp::repair");
  my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "REPAIR is not supported");
  DBUG_RETURN(HA_ERR_UNSUPPORTED);

  // DBUG_RETURN(HA_ADMIN_OK);
}

/*
  Called by the database to lock the table. Keep in mind that this
  is an internal lock.
*/
THR_LOCK_DATA **ha_warp::store_lock(THD *, THR_LOCK_DATA **to,
                                    enum thr_lock_type lock_type) {
  DBUG_ENTER("ha_warp::store_lock");
  if(lock_type != TL_IGNORE && lock.type == TL_UNLOCK) {
    lock.type = lock_type;
  }

  *to++ = &lock;
  DBUG_RETURN(to);
}

int file_exists(const char *file_name) {
  struct stat buf;
  return (stat(file_name, &buf) == 0);
}

int file_exists(std::string file_name) {
  return file_exists(file_name.c_str());
}

void ha_warp::create_writer(TABLE *table_arg) {
  if(writer != NULL) {
    // delete writer;
    return;
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
        if(is_unsigned) {
          datatype = ibis::UBYTE;
        } else {
          datatype = ibis::BYTE;
        }
        break;

      case MYSQL_TYPE_SHORT:
        if(is_unsigned) {
          datatype = ibis::USHORT;
        } else {
          datatype = ibis::SHORT;
        }
        break;

      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
        if(is_unsigned) {
          datatype = ibis::UINT;
        } else {
          datatype = ibis::INT;
        }
        break;

      case MYSQL_TYPE_LONGLONG:
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
        datatype = ibis::TEXT;
        break;

      case MYSQL_TYPE_FLOAT:
        datatype = ibis::FLOAT;
        break;

      case MYSQL_TYPE_DOUBLE:
        datatype = ibis::DOUBLE;
        break;

      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
        datatype = ibis::TEXT;
        break;

      case MYSQL_TYPE_YEAR:
        datatype = ibis::UBYTE;
        break;

      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
        datatype = ibis::UINT;
        break;

      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_DATETIME2:

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
        break;
    }
    /* Fastbit supports numerous bitmap index options.  You can place these
       options in the comment string of a column.  When an index is created on a
       column, then the indexing options used are taken from the comment.  In
       the future, the comment will support more options for compression, etc.
    */
    std::string comment((*field)->comment.str, (*field)->comment.length);
    std::string index_spec = "";
    if(comment.substr(0, 6) != "index=") {
      comment = "";
      index_spec = comment;
    }
    writer->addColumn(name.c_str(), datatype, comment.c_str(),
                      index_spec.c_str());

    /* Columns which are NULLable have a NULL marker.  A better approach might
       to have one NULL bitmap stored as a separate column instead of one byte
       per NULLable column, but that makes query processing a bit more complex
       so this simpler approach is taken for now.  Also, once compression is
       implemented, these columns will shrink quite a bit.
    */
    if(is_nullable) {
      // writer->addColumn(nname.c_str(),ibis::BIT,"NULL bitmap the
      // correspondingly numbered column");
      writer->addColumn(nname.c_str(), ibis::UBYTE,
                        "NULL marker for the correspondingly numbered column");
    }
  }
  /* This is the psuedo-rowid which is used for deletes and updates */
  writer->addColumn("r", ibis::ULONG, "WARP rowid");

    /* This is the psuedo-rowid which is used for deletes and updates */
  writer->addColumn("t", ibis::ULONG, "WARP transaction identifier");

  /* This is the memory buffer for writes*/
  writer->reserveBuffer(my_write_cache_size > my_partition_max_rows
                            ? my_partition_max_rows
                            : my_write_cache_size);

  /* FIXME: should be a table option and should be able to be set in size not
   * just count*/
  writer->setPartitionMax(my_partition_max_rows);
  mysql_mutex_lock(&share->mutex);
  if(share->next_rowid == 0) {
    auto tbl = new ibis::mensa(share->data_dir_name);
    share->next_rowid = tbl->nRows() + 1;
    delete tbl;
  }
  mysql_mutex_unlock(&share->mutex);
}

/*
  Create a table. You do not want to leave the table open after a call to
  this (the database will call ::open() if it needs to).

  Note that the internal Fastbit columns are named after the field numbers
  in the MySQL table.
*/
// int ha_warp::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *info,
int ha_warp::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *,
                    dd::Table *) {
  DBUG_ENTER("ha_warp::create");
  int rc = 0;
  if(!(share = get_share(name, table))) DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  /* create the writer object from the list of columns in the table */
  create_writer(table_arg);

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

bool ha_warp::check_if_incompatible_data(HA_CREATE_INFO *, uint) {
  return COMPATIBLE_DATA_YES;
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
  current_rowid = 0;
  /* When scanning this is used to skip evaluation of transactions
     that have already been evaluated
  */  
  last_trx_id = 0;
  current_trx = NULL;

  /* This is the a big part of the performance advantage of WARP outside of
     the bitmap indexes.  This figures out which columns this query is reading
     so we only read the necesssary columns from disk.

     This is the primary advantage of a column store.
  */
  set_column_set();

  /* This second table object is a new relation which the projection of all
     the rows and columns in the table.

     base_table may already be initialized if an index read is going to
     call position a row after an index scan.
  */
  if(base_table == NULL) {
    base_table = new ibis::mensa(share->data_dir_name);
  }

  /* push_where_clause is populated in ha_warp::cond_push() which is the
     handler function invoked by engine condition pushdown.  When ECP is
     used, then push_where_clause will be a non-empty string.  If it
     isn't used, then the WHERE clause is set such that Fastbit will
     return all rows.
  */
  if(push_where_clause == "") {
    push_where_clause = "1=1";
  }

  filtered_table =
      base_table->select(column_set.c_str(), push_where_clause.c_str());
  if(filtered_table != NULL) {
    /* Allocate a cursor for any queries that actually fetch columns */
    cursor = filtered_table->createCursor();
  }

  DBUG_RETURN(0);
}

/* Moves to the next row in the current cursor which is either the whole table
   or a subset of rows based on a pushed WHERE condition.

   Returns HA_ERR_END_OF_FILE when there are no rows remaining in the cursor.

   This function (and also other functions which populate rows) must check that
   the fetched row is visible.  In the future this will include transaction
   visibility, but right now it only involves checking that the row is not
   deleted.  If the row is deleted, another fetch must be attempted.
*/
int ha_warp::rnd_next(uchar *buf) {
  DBUG_ENTER("ha_warp::rnd_next");
  uint64_t row_trx_id = 0;
  
  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);

  if(cursor == NULL || cursor->nRows() <= 0) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }

fetch_again:
  if(cursor->fetch() != 0) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  
  cursor->getColumnAsULong("t", row_trx_id);
  /* if we already checked this trx_id in the last iteration
     then it does not have to be checked again and the
     is_visible variable does not change
  */
  
  if(!is_visible_to_read(row_trx_id)) {
    dbug("last_trx_id: " + std::to_string(last_trx_id) + " current_trx_id: " + std::to_string(current_trx->trx_id));
    goto fetch_again;
  } else {
    dbug("last_trx_id: " + std::to_string(last_trx_id) + " current_trx_id: " + std::to_string(current_trx->trx_id));
    is_visible = true;
  }
   
  dbug("READ_RND: trx_id:" + std::to_string(current_trx->trx_id) + " row_trx_id: " + std::to_string(row_trx_id) + " is_visible:" + std::to_string(is_visible));
  
  if(!is_visible) {
    goto fetch_again;
  }

  cursor->getColumnAsULong("r", current_rowid);
  if(is_deleted(current_rowid)) {
    goto fetch_again;
  }

  find_current_row(buf, cursor);

  DBUG_RETURN(0);
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
  uint64_t rownum = 0;
  assert(cursor);
  rownum = cursor->getCurrentRowNumber();
  my_store_ptr(ref, ref_length, rownum);
  DBUG_VOID_RETURN;
}

/*
  Used to seek to a logical posiion stored with ::position().
*/
int ha_warp::rnd_pos(uchar *buf, uchar *pos) {
  int rc;
  DBUG_ENTER("ha_warp::rnd_pos");
  ha_statistic_increment(&System_status_var::ha_read_rnd_count);
  auto current_rownum = my_get_ptr(pos, ref_length);
  cursor->fetch(current_rownum);
  rc = find_current_row(buf, cursor);
  cursor->getColumnAsULong("r", current_rowid);
  DBUG_RETURN(rc);
}

/*
  Called after each table scan. In particular after deletes,
  and updates.
*/
int ha_warp::rnd_end() {
  DBUG_ENTER("ha_warp::rnd_end");
  blobroot.Clear();

  delete cursor;
  cursor = NULL;
  delete filtered_table;
  filtered_table = NULL;
  delete base_table;
  base_table = NULL;
  push_where_clause = "";
  DBUG_RETURN(0);
}

ulong ha_warp::index_flags(uint, uint, bool) const {
  // return(HA_READ_NEXT | HA_READ_RANGE | HA_KEYREAD_ONLY |
  // HA_DO_INDEX_COND_PUSHDOWN);
  return (HA_READ_NEXT | HA_READ_RANGE | HA_KEYREAD_ONLY);
}

/* FIXME: quite sure this is not implemented correctly, but I always want to
   prefer indexes over table scans right now, and prefer unique indexes
   over non-unique indexes.  Everything is going to be pushed down with ECP
   anyway...
*/
ha_rows ha_warp::records_in_range(uint, key_range *, key_range *) {
  /* We want the optimizer to prefer hash joins because nested loop
      joins are expensive and we pretty much always want hash joins.
      Right now, every index just returns the number of rows in the table, so
     the optimizer should join tables purely based on table size.  Of course,
     indexes may reduce the number of rows scanned but this is purely an
     estimate to get the optimizer to join by table size right now.
  */
  if(base_table == NULL) {
    base_table = new ibis::mensa(share->data_dir_name);
  }
  auto cnt = base_table->nRows();
  delete base_table;
  base_table = NULL;
  return cnt;
  /*
  this is a real implemenation that uses Fastbit's estimate function, but it
  appears to result in the same result as the fake implementation and is
  more expensive
  key_range mintmp,maxtmp;
  mintmp = *min;
  mintmp.flag = HA_READ_KEY_OR_NEXT;
  maxtmp = *max;
  maxtmp.flag = HA_READ_KEY_OR_PREV;
  std::string where_clause;
  make_where_clause(mintmp.key, mintmp.keypart_map, mintmp.flag, where_clause);
  make_where_clause(maxtmp.key, maxtmp.keypart_map, maxtmp.flag, where_clause);
  uint64_t int_min, int_max;
  if(base_table == NULL) {
    base_table = new ibis::mensa(share->data_dir_name);
  }
  assert(base_table != NULL);
  base_table->estimate(where_clause.c_str(), int_min, int_max);
  return int_max;
  */
}

int ha_warp::index_init(uint idxno, bool) {
  DBUG_ENTER("ha_warp::index_init");
  // just prevents unused variable warning
  //if(sorted) sorted = sorted;

  /*FIXME: bitmap indexes are not sorted so figure out what the sorted arg
   * means...*/
  //assert(!sorted);

  //DBUG_PRINT("ha_warp::index_init", ("Key #%d, sorted:%d", idxno, sorted));
  DBUG_RETURN(index_init(idxno));
}

int ha_warp::index_init(uint idxno) {
  active_index = idxno;
  last_trx_id = 0;
  current_trx = NULL;

  if(base_table == NULL) {
    base_table = new ibis::mensa(share->data_dir_name);
  }

  if(column_set == "") {
    set_column_set();
  }
  return (0);
}

int ha_warp::index_next(uchar *buf) {
  DBUG_ENTER("ha_warp::index_next");
  uint64_t row_trx_id;
  
fetch_again:
  ha_statistic_increment(&System_status_var::ha_read_next_count);
  if(idx_cursor->fetch() != 0) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  idx_cursor->getColumnAsULong("t", row_trx_id);
  if(!is_visible_to_read(row_trx_id)) {
    goto fetch_again;
  }
  
  idx_cursor->getColumnAsULong("r", current_rowid);
  if(is_deleted(current_rowid)) {
    goto fetch_again;
  }
  find_current_row(buf, idx_cursor);
  DBUG_RETURN(0);
}

int ha_warp::index_first(uchar *buf) {
  DBUG_ENTER("ha_warp::index_first");
  ha_statistic_increment(&System_status_var::ha_read_first_count);
  uint64_t row_trx_id;
  last_trx_id = 0;
  current_trx = NULL;
  
  set_column_set();
  std::string where_clause;

  if(push_where_clause != "") {
    where_clause = push_where_clause;
  } else {
    where_clause = "1=1";
  }

  idx_filtered_table =
      base_table->select(column_set.c_str(), where_clause.c_str());
  if(idx_filtered_table == NULL) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  idx_cursor = idx_filtered_table->createCursor();
fetch_again:
  if(idx_cursor->fetch() != 0) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  idx_cursor->getColumnAsULong("t", row_trx_id);
  if(!is_visible_to_read(row_trx_id)) {
    goto fetch_again;
  }
  
  idx_cursor->getColumnAsULong("r", current_rowid);
  if(is_deleted(current_rowid)) {
    goto fetch_again;
  }
  find_current_row(buf, idx_cursor);

  DBUG_RETURN(0);
}

int ha_warp::index_end() {
  DBUG_ENTER("ha_warp::index_end");
  if(idx_cursor) {
    delete idx_cursor;
    idx_cursor = NULL;
  }
  if(idx_filtered_table) {
    delete idx_filtered_table;
    idx_filtered_table = NULL;
  }
  if(base_table) {
    delete base_table;
    base_table = NULL;
  }
  push_where_clause = "";
  DBUG_RETURN(0);
}

int ha_warp::make_where_clause(const uchar *key, key_part_map keypart_map,
                               enum ha_rkey_function find_flag,
                               std::string &where_clause) {
  DBUG_ENTER("ha_warp::make_where_clause");
  where_clause.reserve(65535);
  where_clause = "";
  /* If the bit is set, then the part is being used.  Unfortunately MySQL will
     only consider prefixes so we need to use ECP for magical performance.
  */
  auto key_offset = key;
  for (uint16_t partno = 0;
       partno < table->key_info[active_index].actual_key_parts; partno++) {
    /* given index (a,b,c) and where a=1 quit when we reach the b key part
        given a=1 and b=2 then quit when we reach the c part
    */
    if(!(keypart_map & (1 << partno))) {
      DBUG_RETURN(0);
    }
    /* What field is this? */
    Field *f = table->key_info[active_index].key_part[partno].field;

    if(where_clause != "") where_clause += " AND ";

    /* Which column number does this correspond to? */
    where_clause += "c" + std::to_string(table->key_info[active_index]
                                             .key_part[partno]
                                             .field->field_index());

    switch(find_flag) {
      case HA_READ_AFTER_KEY:
        where_clause += " > ";
        break;
      case HA_READ_BEFORE_KEY:
        where_clause += " < ";
        break;
      case HA_READ_KEY_EXACT:
        where_clause += " = ";
        break;
      case HA_READ_KEY_OR_NEXT:
        where_clause += ">=";
        break;

      case HA_READ_KEY_OR_PREV:
        where_clause += "<=";
        break;

      case HA_READ_PREFIX:
      case HA_READ_PREFIX_LAST:
      case HA_READ_PREFIX_LAST_OR_PREV:
      default:
        DBUG_RETURN(-1);
    }

    bool is_unsigned = f->all_flags() & UNSIGNED_FLAG;

    switch(f->real_type()) {
      case MYSQL_TYPE_TINY:
        if(is_unsigned) {
          where_clause += std::to_string((uint8_t)(*key_offset));
        } else {
          where_clause += std::to_string((int8_t)(*key_offset));
        }
        key_offset += 1;
        break;

      case MYSQL_TYPE_SHORT:

        if(is_unsigned) {
          where_clause +=
              std::to_string((uint16_t)(*(const uint16_t *)key_offset));
        } else {
          where_clause +=
              std::to_string((int16_t)(*(const int16_t *)key_offset));
        }
        key_offset += 2;
        break;

      case MYSQL_TYPE_LONG:
        if(is_unsigned) {
          where_clause +=
              std::to_string((uint32_t)(*(const uint32_t *)key_offset));
        } else {
          where_clause +=
              std::to_string((int32_t)(*(const int32_t *)key_offset));
        }
        key_offset += 4;
        break;

      case MYSQL_TYPE_LONGLONG:
        if(is_unsigned) {
          where_clause +=
              std::to_string((uint64_t)(*(const uint64_t *)key_offset));
        } else {
          where_clause +=
              std::to_string((int64_t)(*(const int64_t *)key_offset));
        }
        key_offset += 8;

        break;

      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_JSON:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET: {
        // for strings, the key buffer is fixed width, and there is a two byte
        // prefix which lists the string length
        uint16_t strlen = (uint16_t)(*(const uint16_t *)key_offset);
        std::string escaped;
        for (unsigned int i = 0; i < strlen; ++i) {
          char c = *(key_offset + 2 + i);
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
        where_clause += "'" + escaped + "'";

        key_offset +=
            table->key_info[active_index].key_part[partno].store_length;
        break;
      }

      case MYSQL_TYPE_FLOAT:
        where_clause += std::to_string((float_t)(*(const float_t *)key_offset));
        key_offset += 4;
        break;

      case MYSQL_TYPE_DOUBLE:
        where_clause +=
            std::to_string((double_t)(*(const float_t *)key_offset));
        key_offset += 8;
        break;

        // Support lookups for these types

      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIME2: {
        where_clause += std::to_string(f->val_int());
        key_offset += f->data_length();
      }

      break;

      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_GEOMETRY:
      default:
        /* FIXME: indexing these types doesn't work yet! */
        sql_print_error("Internal error: unsupported data type found in WARP table");
        assert(false);
        key_offset +=
            table->key_info[active_index].key_part[partno].store_length;

        break;
    }

    /* exclude NULL columns */
    // if(f->is_nullable()) {
    //  where_clause += " and n" + std::to_string(f->field_index()) + " = 0";
    //}
  }
  DBUG_PRINT("ha_warp::make_where_clause",
             ("WHERE CLAUSE: %s", where_clause.c_str()));
  DBUG_RETURN(0);
}

int ha_warp::index_read_map(uchar *buf, const uchar *key,
                            key_part_map keypart_map,
                            enum ha_rkey_function find_flag) {
  DBUG_ENTER("ha_warp::index_read_map");
  ha_statistic_increment(&System_status_var::ha_read_key_count);
  // DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  std::string where_clause;
  uint64_t row_trx_id;
  last_trx_id = 0;
  current_trx = NULL;
    
  set_column_set();

  make_where_clause(key, keypart_map, find_flag, where_clause);
  if(push_where_clause != "") {
    where_clause = push_where_clause + " AND (" + where_clause + ")";
  }

  if(idx_cursor) {
    delete idx_cursor;
    idx_cursor = NULL;
    delete idx_filtered_table;
    idx_filtered_table = NULL;
  }

  if(base_table == NULL) {
    base_table = new ibis::mensa(share->data_dir_name);
  }
  if(base_table == NULL) {
    sql_print_error("Could not open table", share->data_dir_name);
    assert(false);
  }

  idx_filtered_table =
      base_table->select(column_set.c_str(), where_clause.c_str());
  if(idx_filtered_table == NULL) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }

  idx_cursor = idx_filtered_table->createCursor();
  if(!idx_cursor) {
    DBUG_RETURN(-1);
  }
fetch_again:
  if(idx_cursor->fetch() != 0) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  idx_cursor->getColumnAsULong("t", row_trx_id);
  if(!is_visible_to_read(row_trx_id)) {
    goto fetch_again;
  }
  
  idx_cursor->getColumnAsULong("r", current_rowid);
  if(is_deleted(current_rowid)) {
    goto fetch_again;
  }

  find_current_row(buf, idx_cursor);
  delete base_table;
  base_table = NULL;
  DBUG_RETURN(0);
}

int ha_warp::index_read_idx_map(uchar *buf, uint idxno, const uchar *key,
                                key_part_map keypart_map,
                                enum ha_rkey_function find_flag) {
  DBUG_ENTER("ha_warp::index_read_idx_map");
  std::string where_clause = "";
  auto save_idx = active_index;
  active_index = idxno;
  int rc = index_read_map(buf, key, keypart_map, find_flag);
  active_index = save_idx;
  // index_end();
  DBUG_RETURN(rc);
}

/**
 * This function replaces (for SELECT queries) the handler::cond_push
 * function.  Instead of using an array of ITEM* it uses an
 * abstract query plan.
 * This function now calls ha_warp::cond_push to do the work that it used
 * to do in 8.0.20
 * @param table_aqp The specific table in the join plan to examine.
 * @return Possible error code, '0' if no errors.
 */
int ha_warp::engine_push(AQP::Table_access *table_aqp) {
  DBUG_TRACE;
  const Item *remainder = NULL;
  push_where_clause = "";

  THD const *thd = table->in_use;
  if(thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) {
    const Item *cond = table_aqp->get_condition();
    if(cond == nullptr) return 0;

    /*
      If a join cache is referred by this table, there is not a single
      specific row from the 'other tables' to compare rows from this table
      against. Thus, other tables can not be referred in this case.
    */
    const bool other_tbls_ok =
        thd->lex->sql_command == SQLCOM_SELECT && !table_aqp->uses_join_cache();

    /* Push condition to handler, possibly leaving a remainder */
    remainder = cond_push(cond, other_tbls_ok);

    table_aqp->set_condition(const_cast<Item *>(remainder));
  }
  return 0;
}

/* This is the ECP (engine condition pushdown) handler code.  This is where the
   WARP magic really happens from a MySQL standpoint, since it allows index
   usage that MySQL would not normally support and provides automatic indexing
   for filter conditions.

   This code is called from ha_warp::engine_push in 8.0.20+
*/
const Item *ha_warp::cond_push(const Item *cond, bool other_tbls_ok) {
  /* only pushdown for SELECT */
  if(lock.type != TL_READ) return cond;

  // return cond;
  std::string where_clause;

  /* A simple comparison without conjuction or disjunction */
  if(cond->type() == Item::Type::FUNC_ITEM) {
    if(!append_column_filter(cond, where_clause)) {
      push_where_clause = "";
      return cond;
    }
    /* List of connected simple conditions */
  } else if(cond->type() == Item::Type::COND_ITEM) {
    auto item_cond = (dynamic_cast<Item_cond *>(const_cast<Item *>(cond)));
    List<Item> *items = item_cond->argument_list();
    auto cnt = items->size();

    push_where_clause += '(';
    for (uint i = 0; i < cnt; ++i) {
      auto item = items->pop();
      if(i > 0) {
        if(item_cond->functype() == Item_func::Functype::COND_AND_FUNC) {
          push_where_clause += ") AND (";
        } else if(item_cond->functype() == Item_func::Functype::COND_OR_FUNC) {
          push_where_clause += ") OR (";
        } else {
          push_where_clause = "";
          /* not handled */
          return cond;
        }
      }
      /* recurse to print the field and other items.  This should be a
         FUNC_ITEM. if it isn't, then the item will be returned by this function
         and pushdown evaluation will be abandoned.
      */
      if(cond_push(item, other_tbls_ok) != NULL) {
        push_where_clause = "";
        return cond;
      }
    }
    push_where_clause += ")";
  }
  push_where_clause += where_clause;

  return NULL;
}

/* return 1 if this clause could not be processed (will be processed by MySQL)*/
bool ha_warp::append_column_filter(const Item *cond,
                                   std::string &push_where_clause) {
  bool field_may_be_null = false;

  if(cond->type() == Item::Type::FUNC_ITEM) {
    bool is_between = false;
    bool is_in = false;
    bool is_is_null = false;
    bool is_isnot_null = false;
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
        return false;
    }

    Item **arg = tmp->arguments();
    if(tmp->arg_count == 2 && arg[0]->type() == Item::Type::FIELD_ITEM &&
        arg[0]->type() == arg[1]->type()) {
      /* can't push down joins just yet */
      return false;
    }

    for (uint i = 0; i < tmp->arg_count; ++i, ++arg) {
      if(i > 0) {
        if(!is_between && !is_in) { /* normal <, >, =, LIKE, etc */
          push_where_clause += op;
        } else {
          if(is_between) {
            if(i == 1) {
              push_where_clause += " BETWEEN ";
            } else {
              push_where_clause += " AND ";
            }
          } else {
            if(is_in) {
              if(i == 1) {
                push_where_clause += " IN (";
              } else {
                push_where_clause += ", ";
              }
            }
          }
        }
      }

      /* TODO:
         While there are some Fastbit functions that could be pushed down
         we don't handle that yet, but put this here as a reminder that it
         can be done at some point, as it will speed things up.
      */
      if((*arg)->type() == Item::Type::FUNC_ITEM) {
        if((dynamic_cast<Item_func *>(*arg))->functype() ==
            Item_func::DATE_FUNC) {
          push_where_clause += std::to_string((*arg)->val_uint());
          continue;
        } else {
          push_where_clause = "";
          return false;
        }
      }
      if((*arg)->type() == Item::Type::INT_ITEM) {
        push_where_clause += std::to_string((*arg)->val_int());
        continue;
      }

      if((*arg)->type() == Item::Type::NULL_ITEM) {
        push_where_clause += " NULL ";
        continue;
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
          if(field_may_be_null) {
            push_where_clause +=
                "(n" + std::to_string(field_index) + " = 0 AND ";
          }
          push_where_clause += "c" + std::to_string(field_index);
        } else {
          /* Handle IS NULL and IS NOT NULL, depending on NULLability */
          if(field_may_be_null) {
            if(is_is_null) {
              /* the NULL marker will be one if the value is NULL */
              push_where_clause += "(n" + std::to_string(field_index) + " = 1";
            } else if(is_isnot_null) {
              /* the NULL marker will be zero if the value is NOT NULL */
              push_where_clause += "(n" + std::to_string(field_index) + " = 0";
            }
          } else {
            if(is_is_null) {
              /* NOT NULL field is being compared with IS NULL so no rows can
               * match */
              push_where_clause += " 1=0 ";
            } else if(is_isnot_null) {
              /* NOT NULL field is being compared with IS NOT NULL so all rows
               * match */
              push_where_clause += " 1=1 ";
            }
          }
        }
        continue;
      }

      if((*arg)->type() == Item::Type::STRING_ITEM ||
          (*arg)->type() == Item::Type::DECIMAL_ITEM ||
          (*arg)->type() == Item::Type::REAL_ITEM ||
          (*arg)->type() == Item::Type::VARBIN_ITEM) {
        String s;
        String *val = (*arg)->val_str(&s);
        std::string escaped;
        // char *ptr = val->data_ptr();
        // char *ptr = val->ptr;
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
        push_where_clause += "'" + escaped + "'";
        continue;
      }

      return false;
    }

    if(is_in) {
      push_where_clause += ')';
    }

    if(field_may_be_null) {
      push_where_clause += ')';
    }
  }

  return true;
}


/* Transaction support functions
   TODO: describe WARP transactions
*/
int ha_warp::start_stmt(THD *thd, thr_lock_type lock_type) {
  /*
  fprintf(stderr, "ENTER START_STMT\n");
  int trx_state = 0;
  int retval = 0;
  uint slot = warp_hton->slot;
  Ha_data* ha_data = thd->get_ha_data(slot);
  
  warp_trx *trx= (warp_trx*)ha_data->ha_ptr;
  
  if (trx == NULL) { 
    fprintf(stderr, "WARP: create new trx\n");
    trx = new warp_trx;
    ha_data->ha_ptr = (void*)trx;
  }
  
  trx_state = trx->begin();
  if(trx_state == 0) {
    trans_register_ha(thd, true, warp_hton, const_cast<ulonglong*>(&(trx->trx_id)));
    fprintf(stderr, "WARP START_STMT: created trx!\n");
  } else if(trx_state == 1) {
    //txn->stmt= txn->new_savepoint();
    trans_register_ha(thd, false, warp_hton, const_cast<ulonglong*>(&(trx->trx_id)));
    fprintf(stderr, "WARP START_STMT: continue trx!\n");
  } else {
    retval = -1;
  }
  return retval;
  */
  return -1;
}

int ha_warp::register_trx_with_mysql(THD* thd, warp_trx* trx) {
  long long all_trx = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN | OPTION_TABLE_LOCK);
  trans_register_ha(thd, all_trx > 0, warp_hton, const_cast<ulonglong*>(&(trx->trx_id)));
  return 0;
}

int ha_warp::external_lock(THD *thd, int lock_type){ 
  DBUG_TRACE;
  DBUG_PRINT("enter", ("lock_type: %d", lock_type));
  int trx_state = 0;
  int retval = 0;
  
  trx_state = get_or_create_trx(thd, current_trx);

  if (lock_type != F_UNLCK)  {
    current_trx->lock_count++;

    if(trx_state >= 0) {
      register_trx_with_mysql(thd, current_trx);
    } else {
      /* FAILED TO START TRX */
      retval = -1;
    }
  } else {
      current_trx->lock_count--;
      /*
      if(trx->lock_count == 0) {
        if(!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN | OPTION_TABLE_LOCK)) {
          warp_commit(warp_hton, thd, true);
        } 
      }
      */
  }

  return retval;
}

int ha_warp::get_or_create_trx(THD* thd, warp_trx* &trx) {
  if(current_trx != NULL) {
    trx = current_trx;
  } else {
    trx = (warp_trx*)thd->get_ha_data(warp_hton->slot)->ha_ptr;
    if(trx == NULL) { 
      trx = new warp_trx;
      trx->isolation_level = thd_get_trx_isolation(thd);
      thd->get_ha_data(warp_hton->slot)->ha_ptr = (void*)trx;
      trx->thd = thd;
      trx->hton = warp_hton;
      current_trx = trx;
    } 
    trx->begin();
    trx->autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN | OPTION_TABLE_LOCK);
  }
  enum_sql_command sql_command = (enum_sql_command)thd_sql_command(thd);
  if(sql_command == SQLCOM_UPDATE || 
     sql_command == SQLCOM_INSERT ||
     sql_command == SQLCOM_REPLACE ||
     sql_command == SQLCOM_DELETE)  
  {  // the first time a data modification statement is encountered
     // the transaction is marked dirty.  Registering the open
     // transaction prevents a transaction from seeing inserts
     // that are not visible to it and to still find duplicate
     // keys in transactions doing concurrent inserts
     if(!trx->dirty) {
       warp_state->register_open_trx(trx->trx_id);
       trx->dirty = true;
     }
     
     // used to roll back the deletions in a single DELETE or
     // UPDATE or REPLACE statement
     if(sql_command == SQLCOM_UPDATE || 
        sql_command == SQLCOM_REPLACE ||
        sql_command == SQLCOM_DELETE) {
       warp_state->delete_bitmap->create_savepoint();
     }
  }
  
  return 0;
}

void warp_trx::open_insert_log() {
  if(insert_log == NULL) {
    insert_log_filename = std::to_string(trx_id) + std::string(".txlog");
    insert_log=fopen(insert_log_filename.c_str(), "w");
    if(insert_log == NULL) {
      sql_print_error("Could not open insert_log %s", insert_log_filename.c_str());
      assert(false);
    }
  }
}

void warp_trx::write_insert_log_rowid(uint64_t rowid) {
  int sz = fwrite(&rowid, sizeof(uint64_t), 1, insert_log);
  if(sz == 0 || ferror(insert_log) != 0) {
    sql_print_error("failed to write rowid into insert log: %s", insert_log_filename.c_str());
    assert(false);
  }
}

int warp_trx::begin() {
  int retval = 0;
  if(trx_id == 0) {
    fprintf(stderr, "WARP_TRX: start new transaction\n");
    trx_id = warp_state->get_next_trx_id();
  } else {
    retval = 1;
  }
  fprintf(stderr, "WARP_TRX: trx_id: %s\n", std::to_string(trx_id).c_str()); 
  return retval;
}

void warp_trx::commit() {
  if(dirty) {
    if(warp_state->commit_bitmap->set_bit(trx_id, 1) != 0) {
      sql_print_error("Failed to set bit in commit bitmap %s", warp_state->commit_bitmap->get_fname().c_str());
      assert(false);
    }
    if(warp_state->commit_bitmap->commit() != 0) {
      sql_print_error("Failed to commit bits in commit bitmap %s", warp_state->commit_bitmap->get_fname().c_str());
      assert(false);
    }
  }
  if(insert_log != NULL) {
    fclose(insert_log);
    unlink(insert_log_filename.c_str());
    insert_log = NULL;
  }
  dirty=0;
}

// used when an INSERT statement fails due to duplicate key error
void warp_trx::rollback_inserts() {
  if(insert_log != NULL) {
    uint64_t rollback_insert_rowid = 0;
    fseek(insert_log, 0, SEEK_SET);
    clearerr(insert_log);
    do {
      rollback_insert_rowid = 0;
      fread(&rollback_insert_rowid, sizeof(uint64_t), 1, insert_log);
      if(rollback_insert_rowid > 0) {
        warp_state->delete_bitmap->set_bit(rollback_insert_rowid);
      } 
    } while(!feof(insert_log));
    warp_state->delete_bitmap->commit();
    fclose(insert_log);
    insert_log = NULL;
    unlink(insert_log_filename.c_str());
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
  fprintf(stderr, "WARP_COMMIT: all=%d\n", (int)commit_trx);
  warp_trx* trx = warp_get_trx(hton, thd);
  dbug("TRANSACTIN_ID: " + std::to_string(trx->trx_id));

  if(!trx->autocommit && !commit_trx) {
    return 0;
  }
  if(trx->dirty) {
    trx->commit();
    warp_state->delete_bitmap->commit();
    warp_state->mark_transaction_closed(trx->trx_id);
  }
    
  /* if the transaction (autocommit or multi-statement) was 
     commited to disk, then the transaction information for
     the connection must be destroyed.
  */
  thd->get_ha_data(hton->slot)->ha_ptr = NULL;
  delete trx;
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
  warp_trx* trx = warp_get_trx(hton, thd);
  //if a statement failed, we need to rollback the insertions
  //
  if(trx->autocommit || rollback_trx) {
    if(trx->dirty) {
      // undo the changes and close the bitmap
      warp_state->delete_bitmap->rollback();  

      // remove the transaction from the open list
      warp_state->mark_transaction_closed(trx->trx_id);
    }
    // destroy the transaction
    delete trx;
    thd->get_ha_data(hton->slot)->ha_ptr = NULL;
  } else {
    //statement rollback
    if(trx->dirty) {
      // roll back any deletes in this statement
      warp_state->delete_bitmap->rollback_to_savepoint();
    
      // now we update the delete bitmap to undo the inserts
      if(trx->insert_log != NULL) {
        trx->rollback_inserts();
      }
    }
  }
  
  return 0;
}

bool ha_warp::is_visible_to_read(uint64_t row_trx_id) {
  get_or_create_trx(current_thd, current_trx);
  assert(current_trx != NULL);
  if(last_trx_id != row_trx_id) {
    last_trx_id = row_trx_id;
    if(current_trx->trx_id == row_trx_id || row_trx_id == 0) {
      is_visible = true;
    } else {
      if(
        current_trx->isolation_level == ISO_REPEATABLE_READ
        && row_trx_id > current_trx->trx_id
        // && warp_is_trx_open(row_trx_id)
      ) { 
        is_visible = false;
      } else {
        if(warp_state->commit_bitmap->is_set(row_trx_id)) {
          is_visible = true;
        } else {
          is_visible = false;
        }
      }
    }
  }
  return is_visible;
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
        delete writer;
        std::string column_fname = std::string(datadir) + "/t";
        if(part->nRows() > 0) {
          FILE* cfp=fopen(column_fname.c_str(), "w");
          fseek(cfp, part->nRows()-1, SEEK_SET);
          uint64_t zero = 0;
          fwrite(&zero, 1, sizeof(zero), cfp);
          int ferrno = ferror(cfp);
          if(ferrno != 0) {
            sql_print_error("Failed to zerofill file ", column_fname.c_str());
            assert(false);
          }
          fclose(cfp);
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

  if(!shutdown_ok) {
    DIR *dir = opendir(".");
    struct dirent* ent;
    const char trxlog_file_extension[7] = ".txlog";
    if(dir == NULL) {
      sql_print_error("Could not open directory entry for data directory");
      assert(false);
    }
    // find any insertion logs and remove them - there is no need to roll
    // back the insertions, the transactions associated with them will
    // not be in the commit bitmap and any deletions associated with
    // those transactions will be rolled back automatically when the
    // bitmaps are opened
    while((ent = readdir(dir)) != NULL) {
      // skip the deleted and commit bitmaps
      if(ent->d_name[0] == 'c' || ent->d_name[0] == 'd') {
        char* ext_at = ent->d_name;
        int filename_len = strlen(ent->d_name);
        bool found = false;
        for(int i = 0; i< filename_len; ++i) {
          if(*(ext_at + i) == '.') {
            found = true;
            break;
          }
        }
        if(!found) {
            continue;
        }
        if(strncmp(trxlog_file_extension,ext_at, 6) == 0) {
          // found a txlog to remove
          if(unlink(ent->d_name) != 0) {
            sql_print_error("Could not remove transaction log %s", ent->d_name);
            assert(false);
          }
        }
      }
    }
    
    if(!repair_tables()) {
      assert("Table repair failed. Database could not be initialized");
    }
  } 
  
  // this file will be rewritten at clean shutdown
  unlink(shutdown_clean_file.c_str());
  
  // this will create the commits.warp bitmap if it does not exist
  try {
    commit_bitmap = new sparsebitmap(commit_bitmap_file, LOCK_SH); 
  } catch(...) {
    sql_print_error("Could not open commit bitmap: %s", commit_bitmap_file.c_str());
    assert(false);
  }
  // this will create the commits.warp bitmap if it does not exist
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
  int commit_bitmap_exists = (stat(commit_bitmap_file.c_str(), &st) == 0);
  
  if((state_exists && !commit_bitmap_exists)) {
    sql_print_error("warp_state found but commits.warp is missing! Database can not be initialized.");
    return false;
  } 
  
  if((!state_exists && commit_bitmap_exists)) {
    sql_print_error("WARP commits.warp is found but warp_state is missing! Database can not be initialized.");
    return false;
  } 
  
  ibis::partList parts;
  int has_warp_tables = ibis::util::gatherParts(parts, std::string(".").c_str());
  if(!state_exists && !commit_bitmap_exists && has_warp_tables > 0) {
    sql_print_error("WARP tables found but database state is missing! This may be a beta 1 database. WARP can not be initialized.");
    return false;
  }
  return true;
}

uint64_t warp_global_data::get_next_trx_id() {
  mtx.lock();
  ++next_trx_id;
  write();
  mtx.unlock();
  return next_trx_id;
}

uint64_t warp_global_data::get_next_rowid_batch() {
  mtx.lock();
  next_rowid += 10;
  write();
  mtx.unlock();
  return next_rowid;
}

void warp_global_data::register_open_trx(uint64_t trx_id) {
   mtx.lock();
   open_write_trx.push_front(trx_id);
   mtx.unlock();
}

void warp_global_data::mark_transaction_closed(uint64_t trx_id) {
  mtx.lock();
  open_write_trx.remove(trx_id);
  mtx.unlock();
}

bool warp_global_data::is_transaction_open(uint64_t trx_id) {
  bool retval = false;
  mtx.lock();
  for(auto it = open_write_trx.begin();it != open_write_trx.end();++it) {
    if(*it == trx_id) {
      retval = true;  
      break;
    } 
  }
  mtx.unlock();
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
  ulonglong max_wait_time = THDVAR(trx->thd, lock_wait_timeout);
  ulonglong wait_time = 0;
  // create a new lock for our lock
  // will be deleted and replaced if 
  // we discover we already have this 
  // lock!  
  warp_lock new_lock;
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
    row_locks.insert(std::pair<uint64_t, warp_lock>(rowid, new_lock));
    lock_mtx.unlock();
    return lock_type;
  } else {
    // row is already locked
    for(auto it2 = it;it2 != row_locks.end();++it2) {
      dbug("lock search");
      warp_lock test_lock = it2->second;

      // this lock will be released because of deadlock
      // so go to sleep and wait for it to be released
      // so that we don't possibly hit another deadlock
      // from the same trx before all the locks are 
      // released as the transaction closes
      if(test_lock.lock_type == LOCK_DEADLOCK) {
        dbug("lock is a detected deadlock - waiting for trx rollback to free lock");
        lock_mtx.unlock();
        it2 = it;
        goto sleep;
      }

      // if the row was possibly modified in a newer transaction
      // then return the HISTORY lock.  The row will still be 
      // visible to older read views
      if(trx->trx_id < new_lock.holder && test_lock.lock_type == LOCK_HISTORY) {
        dbug("lock is a history lock - returning lock");
        lock_mtx.unlock();
        return lock_type;
      }

      // the current transaction already holds a lock on this row
      if(test_lock.holder == trx->trx_id) {
        dbug("current trx holds a lock already");
        if(test_lock.waiting_on != 0) {
          dbug("lock is waiting for trx close");
          // does the waiting transaction still exist?
          if(is_transaction_open(test_lock.waiting_on)) {
            dbug("locking trx still exists - waiting");
            lock_mtx.unlock();
            goto sleep;
          }
          dbug("transaction is closed!");
          new_lock.waiting_on = 0;
          row_locks.erase(it);
          row_locks.insert(std::pair<uint64_t, warp_lock>(rowid, new_lock));
          lock_mtx.unlock();
          dbug("returning lock");
          return lock_type;
        }
        if(test_lock.lock_type >= lock_type) {
          dbug("sufficient lock already acquired");
          // this transaction already has a strong enough lock on this row
          // no need to insert the new lock and just return the lock 
          mtx.unlock();
          return lock_type; 
        } 
        dbug("lock upgrade required");
        row_locks.erase(it);
        it2 = it;
        continue;
      }
      
      // this lock is a shared lock by somebody else
      // and this lock request is for a shared lock
      // so keep searching - we will grant the lock request
      // as long as no conflicting EX_LOCK is found
      // AND as long as this lock is not waiting on another
      // transaction
      if(test_lock.lock_type == LOCK_SH && new_lock.lock_type == LOCK_SH) {
        dbug("shared lock on row detected");
        // if the existing  shared lock is not waiting on an EX lock
        // the shared lock can be granted, othewrise
        // we have to wait on this lock
        if(test_lock.waiting_on == 0) {
          dbug("shared lock is not waiting");
          // iterate because this trx might already hold a shared lock 
          // to reuse
          continue;
        }
        // the shared lock is waiting on an EX lock!
        // can not acquire the shared lock right now
        // will sleep a bit if spinlocks are exhausted and 
        // will error out if lock_wait_timeout is exhausted
        dbug("waiting on shared lock");
        new_lock.waiting_on = test_lock.waiting_on;
        row_locks.insert(std::pair<uint64_t, warp_lock>(rowid, new_lock));
        lock_mtx.unlock();
        goto sleep;
      }
      
      // If new_lock points to an existing lock and the 
      // other transaction is already waiting on this
      // lock, then a DEADLOCK is detected!
      // this transaction will be rolled back
      if(lock_type == LOCK_EX && test_lock.waiting_on == new_lock.holder) {
        dbug("deadlock detected");
        new_lock.lock_type = LOCK_DEADLOCK;
        row_locks.insert(std::pair<uint64_t, warp_lock>(rowid, new_lock));
        lock_mtx.unlock();
        return LOCK_DEADLOCK;
      }
      dbug("continuing search");  
    }
    dbug("search completed!");
  }  
  
  new_lock.waiting_on = 0;
  // insert the new lock
  row_locks.insert(std::pair<uint64_t, warp_lock>(rowid, new_lock));
  
  lock_mtx.unlock();
  dbug("returning lock");
  return lock_type;

sleep:
  //fixme - make this configurable
  if(spin_count++ > 0) {
    dbug("sleeping");
    
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
  dbug("retry lock");
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
  fopen(shutdown_clean_file.c_str(), "w");
  if(!sd) {
    sql_print_error("could not open shutdown file");
    assert(false);
  }
  uint8_t one = 1;
  fwrite(&one, sizeof(uint8_t), 1, sd);
  if(ferror(sd) != 0) {
    sql_print_error("could not write shutdown file");
  }
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
  if(fsync(fileno(fp)) != 0) {
    sql_print_error("fsync to database state failed");
    assert(false);
  }
}

warp_global_data::~warp_global_data() {
  if(commit_bitmap->close(1) != 0) {
    sql_print_error("Could not close bitmap %s", commit_bitmap->get_fname().c_str());
    assert(false);
  }
  write();
  fclose(fp);
  commit_bitmap = NULL;
}



