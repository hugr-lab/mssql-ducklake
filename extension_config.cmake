# This file is included by DuckDB's build system. It specifies which extension to load

# The bridge itself. DONT_LINK on purpose: statically linked extensions load at database startup,
# and the bridge's Load fails by contract when ducklake/mssql are absent (specs/001) - every test
# database would die at open. As a loadable it is loaded exactly like production loads it: an
# explicit LOAD, after both sides.
duckdb_extension_load(mssql_ducklake
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    DONT_LINK
    LOAD_TESTS
)

# The two sides of the bridge, at the same release line as the duckdb submodule (v1.5.5).
#
# ducklake is linked statically: the test shell then carries the real metadata-manager registry the
# bridge registers into (the direct-call path of specs/001), and `require ducklake` always holds.
duckdb_extension_load(ducklake
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/ducklake
)

# mssql stays a loadable (DONT_LINK): production pairs the bridge with the distributed artifact, so
# the tests do too - loaded by build path (`LOAD '__BUILD_DIRECTORY__/extension/mssql/...'`). The
# pin is the release tag built against duckdb v1.5.5; its openssl/simdutf arrive through the merged
# vcpkg manifest.
duckdb_extension_load(mssql
    DONT_LINK
    GIT_URL https://github.com/hugr-lab/mssql-extension
    GIT_TAG v0.2.4
)
