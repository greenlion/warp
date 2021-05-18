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

#include "minimal_chassis_runtime_error_imp.h"
#include <mysql/components/service_implementation.h>
#include <stdio.h>

/**
  This is the default implementation for emit api.
  @param error_id    error ID.
  @param flags error flags
  @param args  va_list type, which hold the error message string.
*/

DEFINE_METHOD(void, mysql_runtime_error_imp::emit,
              (int error_id, int flags, va_list args)) {
  char buff[1024];
  size_t len;

  len =
      snprintf(buff, sizeof(buff), "[error ID: %d flag: %d] ", error_id, flags);
  vsnprintf(buff + len, sizeof(buff) - len, "%s", args);

  (void)fflush(stdout);
  (void)fputs(buff, stderr);
  (void)fputc('\n', stderr);
  (void)fflush(stderr);
  return;
}
