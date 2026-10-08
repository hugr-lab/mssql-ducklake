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
# RECON (specs/015): mssql main ed7cb2e - spec 081's given-shape vehicle (`mssql_scan_unsafe`,
# #407), RPC parameters (083), metadata query shapes (084), the native_types ATTACH option, on the
# same duckdb pin (4fbae437b22, #418). Move to a release tag before merge.
duckdb_extension_load(mssql
    GIT_URL https://github.com/hugr-lab/mssql-extension
    GIT_TAG ed7cb2efb4ee3c5a977dc30c3c83bae44f17ef23
)

# RECON (specs/015): the postgres arm of the bench. The 2.0-line duckdb is a dev build with no
# published postgres_scanner, so it is built from source like mssql - a loadable, loaded by build
# path. Not part of the release build. Pinned to duckdb-postgres main, which carries the v2.0-cyanoptera
# patches (Parser's expression list is no longer static there).
duckdb_extension_load(postgres_scanner
    GIT_URL https://github.com/duckdb/duckdb-postgres
    GIT_TAG a0fcfdece407d3450dae7ce19baa1251f11fd356
    SUBMODULES database-connector
)
