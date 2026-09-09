#!/usr/bin/env python3
"""Concurrent writers against one lake: a regression test for specs/007 that sqllogictest cannot be.

The failure this exists for needs two processes committing at the same time, so it cannot live in
the sqllogictest suite at all. What it catches: DuckLake's commit loop, on a retry, used to read
`ducklake_snapshot` twice in one query - the outer scan and a `MAX(snapshot_id)` subquery - and
through an attached catalog those are two separate SELECTs with no consistent read between them. A
commit landing in the gap made them disagree, the conflict check came back without its snapshot row,
and the writer died with "Calling GetValueInternal on a value that is NULL". Six of fourteen rounds
lost a writer before the fix, none after.

Each writer commits into its OWN table, so nothing here is a genuine write conflict: every writer is
expected to finish with all of its rows. A writer that does not is a failure, and the script exits
non-zero.

    make test-concurrent                                  # mssql, the default shape
    make test-concurrent CONCURRENT_ARGS="--backends mssql,postgres --rounds 14"

postgres is worth running beside it when a failure shows up: it is another remote catalog under the
same DuckLake, so a failure on both is DuckLake's and a failure on mssql alone is ours.

The defaults are the ones the failure actually needs. At four writers it did not reproduce in ten
rounds even with the fix disabled, which would have made this a test that cannot fail; at six it
loses writers reliably. To check that for yourself:

    MSSQL_DUCKLAKE_NO_CONFLICT_REWRITE=1 make test-concurrent    # must FAIL
    make test-concurrent                                          # must pass
"""

import argparse
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

LOAD_MSSQL = (
    "LOAD 'build/release/extension/mssql/mssql.duckdb_extension';\n"
    "LOAD 'build/release/extension/mssql_ducklake/mssql_ducklake.duckdb_extension';\n"
)
LOAD_PG = "INSTALL postgres; LOAD postgres;\n" + LOAD_MSSQL

# Drops every ducklake% table in dbo. The integration suite uses the same predicate and the same
# reasoning: within the metadata schema that prefix is DuckLake's own namespace.
MSSQL_RESET = """SELECT mssql_exec('srv', '
DECLARE @sql NVARCHAR(MAX) = N'''';
SELECT @sql += N''DROP TABLE '' + QUOTENAME(s.name) + N''.'' + QUOTENAME(t.name) + N'';''
FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id
WHERE s.name = ''dbo'' AND t.name LIKE ''ducklake%'';
EXEC sp_executesql @sql;');
"""


def run_duckdb(duckdb: str, script: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [duckdb, "-unsigned", "-csv", "-noheader"],
        input=script, capture_output=True, text=True, timeout=600,
    )


def loads(backend: str) -> str:
    return LOAD_MSSQL if backend == "mssql" else LOAD_PG


def attach(backend: str, args, data_path: str, schema: str) -> str:
    if backend == "mssql":
        return f"ATTACH 'ducklake:mssql:{args.mssql_dsn}' AS lake (DATA_PATH '{data_path}');\n"
    return (f"ATTACH 'ducklake:postgres:{args.pg_dsn}' AS lake "
            f"(DATA_PATH '{data_path}', METADATA_SCHEMA '{schema}');\n")


def reset(backend: str, args, data_path: str, schema: str, writers: int) -> None:
    """A fresh catalog and one empty table per writer."""
    if backend == "mssql":
        wipe = LOAD_MSSQL + f"ATTACH '{args.mssql_dsn}' AS srv (TYPE mssql);\n" + MSSQL_RESET
    else:
        wipe = (f"INSTALL postgres; LOAD postgres;\nATTACH '{args.pg_dsn}' AS pg (TYPE postgres);\n"
                f"DROP SCHEMA IF EXISTS pg.{schema} CASCADE;\nCREATE SCHEMA pg.{schema};\n")
    run_duckdb(args.duckdb, wipe)
    tables = "".join(f"CREATE TABLE lake.w{w}(i BIGINT);\n" for w in range(1, writers + 1))
    proc = run_duckdb(args.duckdb, loads(backend) + attach(backend, args, data_path, schema) + tables)
    if proc.returncode != 0:
        sys.exit(f"could not prepare the {backend} lake:\n{proc.stdout}\n{proc.stderr}")


def writer(backend: str, args, data_path: str, schema: str, n: int) -> tuple:
    """One writer: `commits` file-backed commits into its own table, then counts its rows."""
    body = "".join(f"INSERT INTO lake.w{n} SELECT r FROM range({args.rows}) t(r);\n"
                   for _ in range(args.commits))
    script = (loads(backend) + attach(backend, args, data_path, schema) + body
              + f"SELECT count(*) FROM lake.w{n};\n")
    proc = run_duckdb(args.duckdb, script)
    lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
    got = lines[-1] if lines else ""
    want = str(args.rows * args.commits)
    if got == want:
        return (True, "")
    error = next((ln for ln in (proc.stdout + proc.stderr).splitlines() if "Error" in ln), "")
    return (False, error or f"ended with {got!r}, wanted {want}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--mssql-dsn", default=os.environ.get("MSSQL_DUCKLAKE_TEST_DSN", ""))
    parser.add_argument("--pg-dsn", default=os.environ.get("MSSQL_DUCKLAKE_PG_DSN", ""))
    parser.add_argument("--backends", default="mssql", help="comma-separated: mssql, postgres")
    parser.add_argument("--writers", type=int, default=6,
                        help="processes committing at once; four was not enough to surface the "
                             "specs/007 failure reliably, six is")
    parser.add_argument("--commits", type=int, default=10, help="commits per writer")
    parser.add_argument("--rows", type=int, default=50,
                        help="rows per commit; above the inlining limit, so each writes a file")
    parser.add_argument("--rounds", type=int, default=10,
                        help="rounds per backend; the failure is intermittent, so one round proves nothing")
    args = parser.parse_args()

    backends = [b.strip() for b in args.backends.split(",") if b.strip()]
    if unknown := [b for b in backends if b not in ("mssql", "postgres")]:
        sys.exit(f"unknown backend(s): {', '.join(unknown)}")
    if "mssql" in backends and not args.mssql_dsn:
        sys.exit("MSSQL_DUCKLAKE_TEST_DSN is needed (make test-concurrent sets it)")
    if "postgres" in backends and not args.pg_dsn:
        sys.exit("MSSQL_DUCKLAKE_PG_DSN is needed for the postgres arm")

    print(f"{args.writers} writers x {args.commits} file-backed commits each, "
          f"{args.rounds} rounds per backend, each writer into its own table\n")
    failures = {b: 0 for b in backends}
    for rnd in range(1, args.rounds + 1):
        # backends alternate within a round, so a warm or cold server cannot favour one of them
        for backend in backends:
            data_path = tempfile.mkdtemp(prefix=f"ducklake_conc_{backend}_")
            schema = f"conc_{os.getpid()}_{rnd}"
            reset(backend, args, data_path, schema, args.writers)
            with ThreadPoolExecutor(max_workers=args.writers) as pool:
                results = list(pool.map(
                    lambda n: writer(backend, args, data_path, schema, n),
                    range(1, args.writers + 1)))
            ok = sum(1 for good, _ in results if good)
            print(f"  round {rnd:>2}  {backend:<9} {ok}/{args.writers} writers finished with all their rows")
            for good, why in results:
                if not good:
                    failures[backend] += 1
                    print(f"        {why}")

    print()
    bad = [b for b in backends if failures[b]]
    for backend in backends:
        print(f"  {backend}: {failures[backend]} writer(s) lost over "
              f"{args.rounds * args.writers} writer-runs")
    if bad:
        print("\nA lost writer here is a real defect: every writer commits into its own table, so "
              "none of them\nconflict with each other. See specs/007.")
        sys.exit(1)
    print("\nall writers committed everything")


if __name__ == "__main__":
    main()
