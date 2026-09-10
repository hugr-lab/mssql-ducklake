cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME mssql_ducklake)
# The release version mssql_ducklake_version() answers, like the mssql extension's mssql_version():
# a constant, not the git hash DuckDB puts in duckdb_extensions().extension_version. The release
# commit sets it to the tag's version and release.yml refuses a tag that disagrees; the commit
# after a release bumps it to the next version with "-dev" (specs/011).
set(MSSQL_DUCKLAKE_VERSION "0.1.0")

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# The embedded ducklake (specs/002): the whole untouched pinned source, compiled into this
# extension. Its src/CMakeLists.txt collects the object libraries into ALL_OBJECT_FILES, exactly as
# ducklake's own build consumes them; nothing in the submodule is modified.
include_directories(ducklake/src/include)

# Roaring is ducklake's deletion-vector dependency (via vcpkg); include-dir resolution mirrors
# ducklake's own CMakeLists.
find_package(roaring CONFIG REQUIRED)
if(TARGET roaring::roaring-headers)
  get_target_property(ROARING_INCLUDE_DIRS roaring::roaring-headers INTERFACE_INCLUDE_DIRECTORIES)
else()
  get_target_property(ROARING_INCLUDE_DIRS roaring::roaring INTERFACE_INCLUDE_DIRECTORIES)
endif()
if(NOT ROARING_INCLUDE_DIRS OR ROARING_INCLUDE_DIRS STREQUAL "ROARING_INCLUDE_DIRS-NOTFOUND")
  set(ROARING_INCLUDE_DIRS "${roaring_DIR}/../../include")
endif()
include_directories(${ROARING_INCLUDE_DIRS})

add_subdirectory(ducklake/src ${CMAKE_CURRENT_BINARY_DIR}/ducklake_embed)

set(EXTENSION_SOURCES
    src/mssql_ducklake_extension.cpp src/mssql_metadata_manager.cpp src/mssql_catalog_shape.cpp
    src/mssql_server_commit.cpp src/mssql_metadata_queries.cpp src/mssql_commit_batch.cpp
    ${ALL_OBJECT_FILES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_DUCKLAKE_VERSION="${MSSQL_DUCKLAKE_VERSION}")
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE MSSQL_DUCKLAKE_VERSION="${MSSQL_DUCKLAKE_VERSION}")

if(TARGET roaring::roaring-headers)
  target_link_libraries(${EXTENSION_NAME} roaring::roaring roaring::roaring-headers
                        roaring::roaring-headers-cpp)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} roaring::roaring roaring::roaring-headers
                        roaring::roaring-headers-cpp)
else()
  target_link_libraries(${EXTENSION_NAME} roaring::roaring)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} roaring::roaring)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
