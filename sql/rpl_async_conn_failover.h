/* Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.

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

#ifndef RPL_ASYNC_CONN_FAILOVER_H
#define RPL_ASYNC_CONN_FAILOVER_H

#include "sql/rpl_async_conn_failover_table_operations.h"
#include "sql/rpl_mi.h"

/*
  The class is used to connect to new primary/master in case the
  current slave IO connection gets interrupted.
*/
class Async_conn_failover_manager {
 private:
  /*
    Current position in m_master_conn_detail_list list, whose value it will read
    and re-establish connection. It increments this value each time connection
    is unsuccessful.
  */
  uint m_pos{0};

  /* The list of different master connection details. */
  SENDER_CONN_LIST m_master_conn_detail_list{};

 public:
  Async_conn_failover_manager() {}

  Async_conn_failover_manager(const Async_conn_failover_manager &) = delete;
  Async_conn_failover_manager(Async_conn_failover_manager &&) = delete;
  Async_conn_failover_manager &operator=(const Async_conn_failover_manager &) =
      delete;
  Async_conn_failover_manager &operator=(Async_conn_failover_manager &&) =
      delete;

  /**
    Re-establishes connection to next available primary/master.

    @param[in] mi   the Master_info object of the failed connection which
                    needs to be reconnected to the new primary.

    @retval true   Error connecting to new primary/master.
    @retval false  Success connecting to new primary/master.
 */
  bool do_auto_conn_failover(Master_info *mi);

  /**
    Sets primary network configuration details <host, port, network_namespace>
    for the provided Master_info object. The function is used by async conn
    failure to set configuration details of new primary.

    @param[in] mi   the Master_info object of the failed connection which
                    needs to be reconnected to the new primary.
    @param[in] host the primary hostname to be set for Master_info object
    @param[in] port the primary port to be set for Master_info object
    @param[in] network_namespace the primary network_namespace to be set for
                                 Master_info object

    @retval true   Error
    @retval false  Success
  */
  bool set_channel_conn_details(Master_info *mi, const std::string host,
                                const uint port,
                                const std::string network_namespace);

  /**
    Reset position to start so that all master/primary can be considered on
    next slave IO failure.
  */
  void reset_pos() { m_pos = 0; }
};

#endif /* RPL_ASYNC_CONN_FAILOVER_H */
