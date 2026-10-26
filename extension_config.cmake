# This file is included by DuckDB's build system. It specifies which extension to load

# The extension itself - it EMBEDS ducklake (specs/002), so ducklake must NOT be loaded into this
# build separately: the duplicated functions/ATTACH prefix would collide in the statically linked
# test shell. DONT_LINK because Load runs gates by contract (stock-ducklake exclusion, mssql deps
# check) and a statically linked extension loads at every database startup; tests load it exactly
# like production does, with an explicit LOAD.
duckdb_extension_load(mssql_ducklake
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# The runtime dependency, as a loadable (DONT_LINK): production pairs the extension with the
# distributed mssql artifact, so the tests do too - loaded by build path
# (`LOAD '__BUILD_DIRECTORY__/extension/mssql/...'`); its openssl/simdutf arrive through the merged
# vcpkg manifest. Pinned to mssql-extension main 6df11bc: v0.3.0's code on our duckdb (v2.0.0,
# d54cbbe3df1). Built here from source, so the duckdb commit it names does not have to match ours.
duckdb_extension_load(mssql
    GIT_URL https://github.com/hugr-lab/mssql-extension
    GIT_TAG 6df11bceb032624cbafb4be044a7f3bf5252739a
)

# RECON (specs/015): the postgres arm of the bench. The 2.0-line duckdb is a dev build with no
# published postgres_scanner, so it is built from source like mssql - a loadable, loaded by build
# path. Not part of the release build. Pinned to the duckdb-postgres commit DuckDB's own extension
# config names at our duckdb (c0f5a3c), on the v2.0-cyanoptera line.
# The distribution and release builds (extension-ci-tools' reusable workflow, which sets
# DUCKDB_PLATFORM) leave it out: they ship mssql_ducklake, and postgres_scanner does not even link
# on every platform there (MinGW). So does `make tidy-check` (MSSQL_DUCKLAKE_NO_POSTGRES, see Makefile).
if(NOT DEFINED ENV{DUCKDB_PLATFORM} AND NOT DEFINED ENV{MSSQL_DUCKLAKE_NO_POSTGRES})
  duckdb_extension_load(postgres_scanner
      GIT_URL https://github.com/duckdb/duckdb-postgres
      GIT_TAG c0f5a3c0c4b68b02afbf669d36d7f0e503985c88
      SUBMODULES database-connector
  )
endif()
