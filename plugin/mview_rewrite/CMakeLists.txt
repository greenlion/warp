# Copyright (c) 2026, Justin Swanhart and contributors.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# MVIEW_REWRITE: answers queries from the materialized views of the server.
MYSQL_ADD_PLUGIN(mview_rewrite
  mview_rewrite_plugin.cc
  mv_catalog.cc
  mv_expr.cc
  mv_filter.cc
  mv_match.cc
  MODULE_ONLY
  MODULE_OUTPUT_NAME "mview_rewrite"
  LINK_LIBRARIES extra::rapidjson
  )

IF(NOT TARGET mview_rewrite)
  RETURN()
ENDIF()

TARGET_COMPILE_DEFINITIONS(mview_rewrite PRIVATE
  MYSQL_SERVER LOG_COMPONENT_TAG="mview_rewrite")
