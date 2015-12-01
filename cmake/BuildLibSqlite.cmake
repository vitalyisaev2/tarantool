#
# A macro to build the sqlite submodule
macro(sqlite_build)
#     set(sqlite_src ${PROJECT_SOURCE_DIR}/third_party/sqlite/build/sqlite3.c)

#     add_library(sqlite STATIC ${sqlite_src})
#     set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")
#     set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
	include_directories(${PROJECT_SOURCE_DIR}/third_party/sqlite/build/tsrc)
#     set(LIBSQLITE_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/third_party/sqlite/build/tsrc)
#     set(LIBSQLITE_LIBRARIES sqlite)

#     message(STATUS "Use bundled sqlite library: ${LIBSQLITE_LIBRARIES}")

#     unset(sqlite_src)
endmacro(sqlite_build)

