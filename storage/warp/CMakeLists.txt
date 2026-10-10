# Copyright (c) 2006, 2016, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
include(ExternalProject)

# FastBit compresses column data with zstd.  Use the same zstd as the server.
IF(WITH_ZSTD STREQUAL "system")
  SET(FASTBIT_ZSTD_CPPFLAGS "-I${ZSTD_INCLUDE_DIR}")
  SET(FASTBIT_ZSTD_LIBS "${ZSTD_SYSTEM_LIBRARY}")
ELSE()
  SET(FASTBIT_ZSTD_CPPFLAGS "-I${BUNDLED_ZSTD_PATH}")
  SET(FASTBIT_ZSTD_LIBS "$<TARGET_FILE:zstd>")
ENDIF()

ExternalProject_Add(
  fastbit-2.0.3
  #  URL https://codeforge.lbl.gov/frs/download.php/file/426/fastbit-2.0.3.tar.gz
  #GIT_REPOSITORY https://github.com/greenlion/fastbit
  SOURCE_DIR=fastbit-2.0.3
  # FastBit is linked statically into the server, so only the (position
  # independent) static library is built
  CONFIGURE_COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/src/fastbit-2.0.3/configure --prefix=${CMAKE_CURRENT_SOURCE_DIR} --with-pic --disable-shared CPPFLAGS=${FASTBIT_ZSTD_CPPFLAGS} LIBS=${FASTBIT_ZSTD_LIBS}
  PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/
  BUILD_BYPRODUCTS ${CMAKE_CURRENT_SOURCE_DIR}/lib/libfastbit.a
  #PATCH_COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/patch_fastbit.sh ${CMAKE_CURRENT_SOURCE_DIR}
  
  BUILD_COMMAND make -j6
  BUILD_IN_SOURCE 1
  # rebuild (incrementally) whenever the warp target is built so that
  # changes to the FastBit sources are picked up
  BUILD_ALWAYS 1
)
IF(NOT WITH_ZSTD STREQUAL "system")
  add_dependencies(fastbit-2.0.3 zstd)
ENDIF()

#
#ExternalProject_Add(
#  libSIMDCompressionAndIntersection-external
#  PREFIX=external
#  GIT_REPOSITORY https://github.com/greenlion/SIMDCompressionAndIntersection
#  CONFIGURE_COMMAND /usr/bin/bash -c echo "no configuration step needed"
#  INSTALL_COMMAND /usr/bin/bash -c echo "no install step needed"
#  BUILD_COMMAND make -j12 
#  BUILD_IN_SOURCE 1
#)
#ADD_SUBDIRECTORY(lemire_sorted_simd)

SET(WARP_PLUGIN_STATIC  "warp")
#SET(WARP_PLUGIN_SHARED  "warp_udf")
SET(WARP_PLUGIN_MANDATORY TRUE)

SET(WARP_SOURCES ha_warp.cc ha_warp.h)

MYSQL_ADD_PLUGIN(warp ${WARP_SOURCES} STORAGE_ENGINE MANDATORY)
# The static FastBit library does not contain the libraries it uses, they
# are linked after it
add_library(fastbit STATIC IMPORTED)
set_property(TARGET fastbit PROPERTY IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/lib/libfastbit.a)
set_property(TARGET fastbit PROPERTY INTERFACE_LINK_LIBRARIES ext::zstd rt)
add_dependencies(warp fastbit-2.0.3)

#add_library(libSIMDCompressionAndIntersection STATIC IMPORTED)
#set_property(TARGET libSIMDCompressionAndIntersection PROPERTY IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/libSIMDCompressionAndIntersection-external-prefix/src/libSIMDCompressionAndIntersection-external/libSIMDCompressionAndIntersection.a)
#add_dependencies(libSIMDCompressionAndIntersection libSIMDCompressionAndIntersection-external)
#add_dependencies(warp fastbit libSIMDCompressionAndIntersection)
#target_link_libraries(warp fastbit libSIMDCompressionAndIntersection)
target_link_libraries(warp fastbit)

#MYSQL_ADD_PLUGIN(warp_udf udf.cc
#  MODULE_ONLY MODULE_OUTPUT_NAME "warp_udf")

#add_subdirectory("warp_rewrite")

#install(FILES ${FASTBIT_LIBS} ${CMAKE_BINARY_DIR}/plugin_output_directory/warp_udf.so DESTINATION ${PLUGINDIR})
#install(FILES ${CMAKE_BINARY_DIR}/lemire_sorted_simd-prefix/src/lemire_sorted_simd/libSIMDCompressionAndIntersection.a DESTINATION ${PLUGINDIR})


