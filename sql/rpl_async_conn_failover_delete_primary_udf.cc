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

#include "include/m_string.h"
#include "include/my_dbug.h"
#include "sql/sql_class.h"

#include "sql/auth/auth_acls.h"
#include "sql/rpl_async_conn_failover_delete_primary_udf.h"
#include "sql/rpl_async_conn_failover_table_operations.h"

bool Rpl_async_conn_failover_delete_primary::init() {
  DBUG_TRACE;

  Udf_data udf(m_udf_name, STRING_RESULT,
               Rpl_async_conn_failover_delete_primary::delete_primary,
               Rpl_async_conn_failover_delete_primary::delete_primary_init,
               Rpl_async_conn_failover_delete_primary::delete_primary_deinit);

  m_initialized = !register_udf(udf);
  return !m_initialized;
}

bool Rpl_async_conn_failover_delete_primary::deinit() {
  DBUG_TRACE;
  return !(m_initialized && !unregister_udf(m_udf_name));
}

char *Rpl_async_conn_failover_delete_primary::delete_primary(
    UDF_INIT *, UDF_ARGS *args, char *result, unsigned long *length,
    unsigned char *is_null, unsigned char *error) {
  DBUG_TRACE;
  *is_null = 0;  // result is not null
  *error = 0;

  auto err_val{false};    // error value returned during delete row operation
  std::string err_msg{};  // error message returned during delete row operation

  {
    Rpl_async_conn_failover_table_operations sql_operations(TL_WRITE,
                                                            args->arg_count);

    std::string channel(args->args[0], args->lengths[0]);  // channel name
    std::string host(args->args[1], args->lengths[1]);     // hostname
    uint port = *(reinterpret_cast<long long *>(args->args[2]));  // port

    std::string network_namespace{""};  // network_namespace
    /* if network_namespace is provided */
    if (args->arg_count > 3 && args->lengths[3])
      network_namespace.assign(args->args[3], args->lengths[3]);

    auto primary_conn_details =
        std::make_tuple(0, channel, host, port, network_namespace);

    /* delete row */
    std::tie(err_val, err_msg) =
        sql_operations.delete_row(primary_conn_details);
  }

  my_stpcpy(result, err_msg.c_str());
  *length = err_msg.length();
  return result;
}

bool Rpl_async_conn_failover_delete_primary::delete_primary_init(
    UDF_INIT *init_id, UDF_ARGS *args, char *message) {
  DBUG_TRACE;
  if (args->arg_count < 3) {
    my_stpcpy(message,
              "Wrong arguments: You need to specify all mandatory "
              "arguments.");
    return true;
  }

  if (args->arg_count > 4) {
    my_stpcpy(message, "Wrong arguments: You must specify max 4 arguments.");
    return true;
  }

  if (args->arg_type[0] != STRING_RESULT) {
    my_stpcpy(message, "Wrong arguments: You need to specify channel name.");
    return true;
  }

  if (args->arg_type[1] != STRING_RESULT || args->lengths[1] == 0) {
    my_stpcpy(message, "Wrong arguments: You need to specify hostname.");
    return true;
  }

  if (args->arg_type[2] != INT_RESULT) {
    my_stpcpy(message, "Wrong arguments: You need to specify value for port.");
    return true;
  }

  if (args->arg_count > 3 && args->arg_type[3] != STRING_RESULT) {
    my_stpcpy(message,
              "Wrong arguments: You need to specify correct value for "
              "network_namespace.");
    return true;
  }

  THD *thd{current_thd};

  if (thd == nullptr) {
    my_stpcpy(message,
              "Error checking the user privileges. Check the log for "
              "more details or restart the server.");
    return true;
  }

  Security_context *sctx = thd->security_context();
  if (!sctx->check_access(SUPER_ACL) &&
      !sctx->has_global_grant(STRING_WITH_LEN("REPLICATION_SLAVE_ADMIN"))
           .first) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             "SUPER or REPLICATION_SLAVE_ADMIN");
    return true;
  }

  if (thd && thd->locked_tables_mode) {
    my_stpcpy(message,
              "Can't execute the given operation because you have"
              " active locked tables.");
    return true;
  }

  if (Udf_charset_service::set_return_value_charset(init_id) ||
      Udf_charset_service::set_args_charset(args))
    return true;

  init_id->maybe_null = 0;
  return false;
}

void Rpl_async_conn_failover_delete_primary::delete_primary_deinit(UDF_INIT *) {
  DBUG_TRACE;
  return;
}
