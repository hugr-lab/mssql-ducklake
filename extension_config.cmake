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
# vcpkg manifest. Pinned to mssql-extension 0.3.0 (main ef1a807, #482) on the same duckdb
# (bde1b27b461): the commit community-extensions' ref_next names for mssql.
duckdb_extension_load(mssql
    GIT_URL https://github.com/hugr-lab/mssql-extension
    GIT_TAG ef1a807ffbdd3c3a375ee10ff63e65f55b8efe7f
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

# The Azure bench image (specs/021) keeps both lakes' parquet in blob storage (`abfss://`). The 2.0
# line has no published azure extension, so the image builds it here - only when asked, never in a
# test, distribution or release build. Pinned to duckdb-azure main, which follows the 2.0 line.
if(DEFINED ENV{MSSQL_DUCKLAKE_BENCH_AZURE})
  duckdb_extension_load(azure
      GIT_URL https://github.com/duckdb/duckdb-azure
      GIT_TAG bc5778ff4acb286b91544d158a50893cbd64db41
  )
endif()
