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
# vcpkg manifest. Pinned to mssql-extension main 3c2661a (#497): 0.3.0 plus the TDS read-path
# fixes (#487), the refusal of MAX values past 2^31-1 bytes (#490) and the empty PLP frame (#497),
# on our duckdb (f8f3f4dcd95). Built here from source, so the duckdb commit it names does not have
# to match ours.
duckdb_extension_load(mssql
    GIT_URL https://github.com/hugr-lab/mssql-extension
    GIT_TAG 3c2661afaf957b7d203917b2cdaa43eb0b80a440
)

# RECON (specs/015): the postgres arm of the bench. The 2.0-line duckdb is a dev build with no
# published postgres_scanner, so it is built from source like mssql - a loadable, loaded by build
# path. Not part of the release build. Pinned to duckdb-postgres main, which carries the v2.0-cyanoptera
# patches (Parser's expression list is no longer static there).
# The distribution and release builds (extension-ci-tools' reusable workflow, which sets
# DUCKDB_PLATFORM) leave it out: they ship mssql_ducklake, and postgres_scanner does not even link
# on every platform there (MinGW).
if(NOT DEFINED ENV{DUCKDB_PLATFORM})
  duckdb_extension_load(postgres_scanner
      GIT_URL https://github.com/duckdb/duckdb-postgres
      GIT_TAG 9671d2c5cd5fa8e3b295ca7f130fa7590e9b4568
      SUBMODULES database-connector
  )
endif()
