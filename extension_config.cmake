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
# (`LOAD '__BUILD_DIRECTORY__/extension/mssql/...'`). The pin is the release tag built against
# the v1.5 line (v0.2.5: the catalog's default schema is dbo, multi-scan plans on a pinned
# connection materialize - spec 003); its openssl/simdutf arrive through the merged vcpkg manifest.
# RECON (specs/015): the manager's scans are on spec 081's given-shape vehicle
# (`mssql_scan_unsafe`), pushed as hugr-lab/mssql-extension#407 - branch spec/081-shape-vehicle,
# head 9f369d5, on top of main 88fe137 (with #406's pushdown). Move to a release tag before merge.
duckdb_extension_load(mssql
    GIT_URL /Users/vgribanov/projects/hugr-lab/mssql-extension
    GIT_TAG 4562504403df1481006070b57e2a844c3c1c6dc6
)

# RECON (specs/015): the postgres arm of the bench. The 2.0-line duckdb is a dev build with no
# published postgres_scanner, so it is built from source like mssql - a loadable, loaded by build
# path. Not part of the release build. Its pin is on duckdb 097ee1d34a (09-30), ours is 4af9740da4:
# one call changed in between (`Parser::ParseExpressionList` is no longer static), patched by hand
# in build/release/_deps/postgres_scanner_extension_fc-src - redo it after a clean build.
duckdb_extension_load(postgres_scanner
    GIT_URL https://github.com/duckdb/duckdb-postgres
    GIT_TAG f9db66ec5a5c35a30ce868d2b7233ffc0a21a08e
    SUBMODULES database-connector
)
