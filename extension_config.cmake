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
# RECON, local only (design/005): the manager's scans are on spec 081's given-shape vehicle
# (`mssql_scan_unsafe`), which lives in a local branch of the sibling repo - a71c57c, on top of
# 998660e's pushdown fix. Neither is pushed. Put the branch back on a published ref before merge.
duckdb_extension_load(mssql
    GIT_URL /Users/vgribanov/projects/hugr-lab/mssql-extension
    GIT_TAG a71c57c
)
