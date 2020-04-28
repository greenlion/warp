/*
   Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.

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

#ifndef TABLE_REPLICATION_ASYNCHRONOUS_CONNECTION_FAILOVER_H
#define TABLE_REPLICATION_ASYNCHRONOUS_CONNECTION_FAILOVER_H

/**
  @file storage/perfschema/table_replication_asynchronous_connection_failover.h
  Table replication_asynchronous_connection_failover (declarations).
*/

#include <sys/types.h>

#include "compression.h"  // COMPRESSION_ALGORITHM_NAME_BUFFER_SIZE
#include "my_base.h"
#include "my_io.h"
#include "mysql_com.h"
#include "sql/rpl_async_conn_failover_table_operations.h"
#include "sql/rpl_info.h" /* CHANNEL_NAME_LENGTH*/
#include "storage/perfschema/pfs_engine_table.h"
#include "storage/perfschema/table_helper.h"

class Field;
class Master_info;
class Plugin_table;
struct TABLE;
struct THR_LOCK;

/**
  @addtogroup performance_schema_tables
  @{
*/

/**
  A row in the table. The fields with string values have an additional
  length field denoted by \<field_name\>_length.
*/
struct st_row_rpl_async_conn_failover {
  char channel_name[CHANNEL_NAME_LENGTH];
  uint channel_name_length;
  char host[HOSTNAME_LENGTH];
  uint host_length;
  long port;
  char network_namespace[NAME_LEN];
  uint network_namespace_length;
  uint weight;
};

class PFS_index_rpl_async_conn_failover : public PFS_engine_index {
 public:
  PFS_index_rpl_async_conn_failover()
      : PFS_engine_index(&m_key_1, &m_key_2, &m_key_3, &m_key_4),
        m_key_1("CHANNEL_NAME"),
        m_key_2("HOST"),
        m_key_3("PORT"),
        m_key_4("NETWORK_NAMESPACE") {}

  ~PFS_index_rpl_async_conn_failover() {}

  /**
    Match fetched row with searched values.

    @param master_conn_detail  the tuple contains primary network configuration
                               details to be matched.
  */
  virtual bool match(SENDER_CONN_TUPLE master_conn_detail);

 private:
  PFS_key_name m_key_1;  // channel_name key
  PFS_key_name m_key_2;  // host key
  PFS_key_port m_key_3;  // port key
  PFS_key_name m_key_4;  // network_namespace key
};

/**
  Table
  PERFORMANCE_SCHEMA.TABLE_REPLICATION_ASYNCHRONOUS_CONNECTION_FAILOVER.
*/
class table_replication_asynchronous_connection_failover
    : public PFS_engine_table {
  /** Position of a cursor, for simple iterations. */
  typedef PFS_simple_index pos_t;

 private:
  int make_row(SENDER_CONN_TUPLE master_tuple);

  /** Table share lock. */
  static THR_LOCK m_table_lock;

  /** Table definition. */
  static Plugin_table m_table_def;

  /** Current row */
  st_row_rpl_async_conn_failover m_row;

  /** Current position. */
  pos_t m_pos;

  /** Next position. */
  pos_t m_next_pos;

 protected:
  /**
    Read the current row values.

    @param table            Table handle
    @param buf              row buffer
    @param fields           Table fields
    @param read_all         true if all columns are read.
  */
  virtual int read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                              bool read_all);

  table_replication_asynchronous_connection_failover();

 public:
  ~table_replication_asynchronous_connection_failover();

  /** Table share. */
  static PFS_engine_table_share m_share;

  /**
    Open table function.

    @param tbs  Table share object
  */
  static PFS_engine_table *create(PFS_engine_table_share *tbs);

  /**
    Get the current number of rows read.

    @return number of rows read.
  */
  static ha_rows get_row_count();

  /** Reset the cursor position to the beginning of the table. */
  virtual void reset_position(void);

  /**
    Initialize table for random read or scan.

    @param scan  if true: Initialize for random scans through rnd_next()
                 if false: Initialize for random reads through rnd_pos()

    @return Operation status
      @retval 0     Success
      @retval != 0  Error (error code returned)
  */
  virtual int rnd_init(bool scan);

  /**
    Read next row via random scan.

    @return Operation status
      @retval 0     Success
      @retval != 0  Error (error code returned)
  */
  virtual int rnd_next();

  /**
    Read row via random scan from position.

    @param[out] buf  Buffer to read the row into
    @param      pos  Position from position() call

    @return Operation status
      @retval 0     Success
      @retval != 0  Error (error code returned)
  */
  virtual int rnd_pos(const void *pos);

  /**
    Initialize use of index.

    @param idx     Index to use
    @param sorted  Use sorted order

    @return Operation status
      @retval 0     Success
      @retval != 0  Error (error code returned)
  */
  virtual int index_init(uint idx, bool sorted);

  /**
    Read next row via random scan.

    @param buf  Buffer to read the row into

    @return Operation status
      @retval 0     Success
      @retval != 0  Error (error code returned)
  */
  virtual int index_next();

 private:
  /* Index object to get match searched values */
  PFS_index_rpl_async_conn_failover *m_opened_index;

  /* Stores the data being read i.e. master connection details. */
  SENDER_CONN_LIST master_conn_detail;

  /* Stores error happened while reading rows */
  bool read_error;

  /* Stores the current number of rows read. */
  static ha_rows num_rows;
};

/** @} */
#endif
