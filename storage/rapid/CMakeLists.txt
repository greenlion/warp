# Copyright (c) 2018, 2025, Oracle and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

DISABLE_MISSING_PROFILE_WARNING()

ADD_DEFINITIONS(-DMYSQL_SERVER)

# DuckDB
# ------
# The sources of DuckDB are not in this tree.  They are downloaded when the
# plugin is built, built with DuckDB's own CMake files as static libraries,
# and the libraries are put into one (libduckdb_bundle.a) that is linked into
# ha_rapid.so.  The engine only uses the C API (duckdb.h).  See README.duckdb.
#
#   -DRAPID_DUCKDB_VERSION=latest   (default) the latest release of DuckDB,
#                                   asked from GitHub when the build directory
#                                   is configured for the first time
#   -DRAPID_DUCKDB_VERSION=v2.0.0   that release
#   -DRAPID_DUCKDB_URL=<url|file>   another place to take the source archive
#                                   of that version from (a mirror, a file)
#
# The release that "latest" was resolved to is kept in the cache
# (RAPID_DUCKDB_TAG): configuring the build directory again does not move to
# a newer release.  Remove it (cmake -URAPID_DUCKDB_TAG .) to ask again.
SET(RAPID_DUCKDB_VERSION "latest" CACHE STRING
  "Release of DuckDB that the RAPID engine is built with: a tag (v2.0.0) or 'latest'")
SET(RAPID_DUCKDB_URL "" CACHE STRING
  "Where to take the source archive of DuckDB from (default: GitHub)")

IF(NOT RAPID_DUCKDB_VERSION STREQUAL "latest")
  SET(RAPID_DUCKDB_TAG "${RAPID_DUCKDB_VERSION}" CACHE INTERNAL "" FORCE)
ELSEIF(NOT RAPID_DUCKDB_TAG OR
       NOT "${RAPID_DUCKDB_TAG_FOR}" STREQUAL "latest")
  SET(RAPID_DUCKDB_LATEST_JSON
    ${CMAKE_CURRENT_BINARY_DIR}/duckdb_latest_release.json)
  FILE(DOWNLOAD https://api.github.com/repos/duckdb/duckdb/releases/latest
    ${RAPID_DUCKDB_LATEST_JSON}
    STATUS RAPID_DUCKDB_LATEST_STATUS TIMEOUT 60)
  LIST(GET RAPID_DUCKDB_LATEST_STATUS 0 RAPID_DUCKDB_LATEST_CODE)
  SET(RAPID_DUCKDB_LATEST_TAG "")
  IF(RAPID_DUCKDB_LATEST_CODE EQUAL 0)
    FILE(READ ${RAPID_DUCKDB_LATEST_JSON} RAPID_DUCKDB_LATEST_TEXT)
    STRING(JSON RAPID_DUCKDB_LATEST_TAG ERROR_VARIABLE RAPID_DUCKDB_JSON_ERROR
      GET "${RAPID_DUCKDB_LATEST_TEXT}" tag_name)
  ENDIF()
  IF(NOT RAPID_DUCKDB_LATEST_TAG MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+")
    MESSAGE(FATAL_ERROR
      "RAPID: cannot find the latest release of DuckDB "
      "(https://api.github.com/repos/duckdb/duckdb/releases/latest: "
      "${RAPID_DUCKDB_LATEST_STATUS}).  Name a release with "
      "-DRAPID_DUCKDB_VERSION=vX.Y.Z, or build without the engine "
      "(-DWITHOUT_RAPID_SECONDARY_STORAGE_ENGINE=1).")
  ENDIF()
  SET(RAPID_DUCKDB_TAG "${RAPID_DUCKDB_LATEST_TAG}" CACHE INTERNAL "" FORCE)
ENDIF()
SET(RAPID_DUCKDB_TAG_FOR "${RAPID_DUCKDB_VERSION}" CACHE INTERNAL "" FORCE)
MESSAGE(STATUS "RAPID: DuckDB ${RAPID_DUCKDB_TAG} (RAPID_DUCKDB_VERSION=${RAPID_DUCKDB_VERSION})")

IF(RAPID_DUCKDB_URL)
  SET(RAPID_DUCKDB_ARCHIVE "${RAPID_DUCKDB_URL}")
ELSE()
  SET(RAPID_DUCKDB_ARCHIVE
    "https://github.com/duckdb/duckdb/archive/refs/tags/${RAPID_DUCKDB_TAG}.tar.gz")
ENDIF()

# One directory for every release: another release is another build.
SET(RAPID_DUCKDB_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/duckdb-${RAPID_DUCKDB_TAG})
SET(RAPID_DUCKDB_SOURCE_DIR ${RAPID_DUCKDB_PREFIX}/src/rapid_duckdb_build)
SET(RAPID_DUCKDB_BINARY_DIR ${RAPID_DUCKDB_PREFIX}/src/rapid_duckdb_build-build)
SET(RAPID_DUCKDB_BUNDLE ${RAPID_DUCKDB_BINARY_DIR}/libduckdb_bundle.a)

# DuckDB is not compiled with the options of the server:
#   - its own warnings and language standard (C++17; gcc 14 crashes on one of
#     its files with the C++23 of the server);
#   - the general dynamic TLS model instead of -ftls-model=initial-exec: the
#     static TLS block of the server has no room for a library of this size
#     ("cannot allocate memory in static TLS block" when the plugin is
#     installed, which needed LD_PRELOAD);
#   - hidden visibility: the third party libraries that DuckDB brings (zstd,
#     mbedtls, miniz, ...) must not be mixed up with the ones of the server;
#   - without jemalloc (DuckDB then uses malloc), the shell and the tests.
INCLUDE(ExternalProject)
ExternalProject_Add(rapid_duckdb_build
  PREFIX ${RAPID_DUCKDB_PREFIX}
  URL ${RAPID_DUCKDB_ARCHIVE}
  DOWNLOAD_NAME duckdb-${RAPID_DUCKDB_TAG}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  CMAKE_ARGS
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_C_VISIBILITY_PRESET=hidden
    -DCMAKE_CXX_VISIBILITY_PRESET=hidden
    -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON
    -DCMAKE_C_FLAGS=-ftls-model=global-dynamic
    -DCMAKE_CXX_FLAGS=-ftls-model=global-dynamic
    -DOVERRIDE_GIT_DESCRIBE=${RAPID_DUCKDB_TAG}
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    -DBUILD_BENCHMARKS=OFF
    -DENABLE_SANITIZER=OFF
    -DENABLE_UBSAN=OFF
    -DENABLE_JEMALLOC=OFF
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${RAPID_DUCKDB_BUNDLE}
  )
# every static library of the build (the core, its third party libraries and
# the extensions that are linked statically) in one
ExternalProject_Add_Step(rapid_duckdb_build bundle
  COMMAND ${CMAKE_COMMAND}
    -DBINARY_DIR=${RAPID_DUCKDB_BINARY_DIR}
    -DBUNDLE=${RAPID_DUCKDB_BUNDLE}
    -DAR=${CMAKE_AR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/duckdb_bundle.cmake
  DEPENDEES build
  BYPRODUCTS ${RAPID_DUCKDB_BUNDLE}
  COMMENT "RAPID: libduckdb_bundle.a"
  )

# the include directory must exist when the build is configured
FILE(MAKE_DIRECTORY ${RAPID_DUCKDB_SOURCE_DIR}/src/include)
ADD_LIBRARY(rapid_duckdb STATIC IMPORTED GLOBAL)
SET_TARGET_PROPERTIES(rapid_duckdb PROPERTIES
  IMPORTED_LOCATION ${RAPID_DUCKDB_BUNDLE}
  INTERFACE_INCLUDE_DIRECTORIES ${RAPID_DUCKDB_SOURCE_DIR}/src/include
  INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS};pthread")
ADD_DEPENDENCIES(rapid_duckdb rapid_duckdb_build)

SET(RAPID_SOURCES ha_rapid.cc ha_rapid.h rapid_binlog_consumer.cc
  rapid_sql.cc rapid_sql.h)

IF(WITH_RAPID_SECONDARY_STORAGE_ENGINE AND
    NOT WITHOUT_RAPID_SECONDARY_STORAGE_ENGINE)
  MYSQL_ADD_PLUGIN(rapid ${RAPID_SOURCES}
    STORAGE_ENGINE
    DEFAULT
    LINK_LIBRARIES rapid_duckdb extra::rapidjson extra::unordered_dense ext::zlib
    )
ELSEIF(NOT WITHOUT_RAPID_SECONDARY_STORAGE_ENGINE)
  MYSQL_ADD_PLUGIN(rapid ${RAPID_SOURCES}
    STORAGE_ENGINE
    MODULE_ONLY
    LINK_LIBRARIES rapid_duckdb extra::rapidjson extra::unordered_dense ext::zlib
    )
ENDIF()

IF(TARGET rapid)
  ADD_DEPENDENCIES(rapid rapid_duckdb_build)
ENDIF()
