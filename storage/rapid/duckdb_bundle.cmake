# Puts all the static libraries of a build of DuckDB into one.
#   cmake -DBINARY_DIR=<build dir of DuckDB> -DBUNDLE=<lib.a> -DAR=<ar> -P duckdb_bundle.cmake
FILE(GLOB_RECURSE RAPID_LIBS ${BINARY_DIR}/*.a)
LIST(FILTER RAPID_LIBS EXCLUDE REGEX "libduckdb_bundle\\.a$")
LIST(SORT RAPID_LIBS)
IF(NOT RAPID_LIBS)
  MESSAGE(FATAL_ERROR "RAPID: no static libraries of DuckDB in ${BINARY_DIR}")
ENDIF()
SET(SCRIPT "CREATE ${BUNDLE}.tmp\n")
FOREACH(LIB ${RAPID_LIBS})
  STRING(APPEND SCRIPT "ADDLIB ${LIB}\n")
ENDFOREACH()
# duckdb_register_static_extensions(), which registers the extensions that are
# linked statically (core_functions: SUM and most of the other functions).
# DuckDB generates it for the programs and the shared library that it links,
# it is in none of the static libraries: the object of the shared library
# (built as position independent code) is added.
FILE(GLOB RAPID_LOADER ${BINARY_DIR}/src/CMakeFiles/duckdb.dir/*static_extension_loader.c.o)
IF(NOT RAPID_LOADER)
  MESSAGE(FATAL_ERROR
    "RAPID: the static extension loader of DuckDB was not found in "
    "${BINARY_DIR}/src/CMakeFiles/duckdb.dir")
ENDIF()
FOREACH(OBJ ${RAPID_LOADER})
  STRING(APPEND SCRIPT "ADDMOD ${OBJ}\n")
ENDFOREACH()
STRING(APPEND SCRIPT "SAVE\nEND\n")
FILE(WRITE ${BUNDLE}.mri "${SCRIPT}")
FILE(REMOVE ${BUNDLE}.tmp)
EXECUTE_PROCESS(COMMAND ${AR} -M
  INPUT_FILE ${BUNDLE}.mri RESULT_VARIABLE RAPID_AR_RESULT)
IF(NOT RAPID_AR_RESULT EQUAL 0)
  MESSAGE(FATAL_ERROR "RAPID: ${AR} -M failed for ${BUNDLE}")
ENDIF()
FILE(RENAME ${BUNDLE}.tmp ${BUNDLE})
LIST(LENGTH RAPID_LIBS RAPID_COUNT)
MESSAGE(STATUS "RAPID: ${BUNDLE} (${RAPID_COUNT} libraries)")
