#!/usr/bin/env python3
"""Compare the two DuckLake metadata backends on the same workload.

specs/004 states the performance target as "not worse than the postgres backend", which until now
was an assertion. One binary drives both: this extension embeds ducklake, so `ducklake:mssql:` uses
the manager in this repository and `ducklake:postgres:` uses the in-tree one, in the same process
against the same DuckDB.

Both catalogs are dropped and rebuilt for every run, so the numbers include initialization - which
is where this manager does the most extra work (its keys, indexes and collations).

    make bench                 # both servers must be up: make docker-up && make bench-up
"""

import argparse
import os
import re
import subprocess
import sys

PHASE = re.compile(r"^phase:(?P<name>[a-z_]+)$")
TIMING = re.compile(r"Run Time \(s\): real (?P<real>[0-9.]+)")


def workload(rows: int, commits: int) -> str:
    """The statements, with a marker before each phase. Metadata cost dominates the small commits;
    the bulk insert and the filtered reads are where the data path and the indexes show up."""
    small = "\n".join(f"INSERT INTO lake.events VALUES ({i}, 'row {i}', {i} * 1.5);" for i in range(commits))
    later = "\n".join(f"INSERT INTO lake.events VALUES ({1000 + i}, 'late {i}', {i});" for i in range(commits))
    return f"""
SELECT 'phase:attach';
{{ATTACH}}
SELECT 'phase:create_table';
CREATE TABLE lake.events(id BIGINT, name VARCHAR, amount DECIMAL(18, 4));
SELECT 'phase:small_commits';
{small}
SELECT 'phase:bulk_insert';
INSERT INTO lake.events SELECT r, 'bulk ' || r, r * 0.25 FROM range({rows}) t(r);
SELECT 'phase:commits_after_bulk';
{later}
SELECT 'phase:point_read';
SELECT count(*) FROM lake.events WHERE name = 'bulk 4242';
SELECT count(*) FROM lake.events WHERE id BETWEEN 100 AND 200;
SELECT 'phase:scan_read';
SELECT count(*), sum(amount) FROM lake.events;
SELECT 'phase:update_delete';
UPDATE lake.events SET name = 'updated' WHERE id = 7;
DELETE FROM lake.events WHERE id = 8;
SELECT 'phase:snapshots';
SELECT count(*) FROM ducklake_snapshots('lake');
SELECT 'phase:reattach';
DETACH lake;
{{ATTACH}}
SELECT count(*) FROM lake.events;
SELECT 'phase:end';
"""


def run(duckdb: str, script: str) -> dict:
    """Run one backend's script, returning seconds per phase."""
    proc = subprocess.run(
        [duckdb, "-unsigned", "-csv", "-noheader", "-cmd", ".timer on"],
        input=script, capture_output=True, text=True, timeout=3600,
    )
    if proc.returncode != 0:
        sys.exit(f"backend run failed:\n{proc.stdout[-4000:]}\n{proc.stderr[-4000:]}")
    phases, current = {}, None
    for line in proc.stdout.splitlines():
        line = line.strip()
        marker = PHASE.match(line)
        if marker:
            current = marker.group("name")
            continue
        timing = TIMING.search(line)
        if timing and current and current != "end":
            phases[current] = phases.get(current, 0.0) + float(timing.group("real"))
    if "attach" not in phases:
        sys.exit(f"no timings parsed - did the run produce output?\n{proc.stdout[-2000:]}")
    return phases


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--mssql-dsn", default=os.environ.get("MSSQL_DUCKLAKE_TEST_DSN", ""))
    parser.add_argument("--pg-dsn", default=os.environ.get("MSSQL_DUCKLAKE_PG_DSN", ""))
    parser.add_argument("--data-path", default="/tmp/ducklake_bench")
    parser.add_argument("--rows", type=int, default=200000)
    parser.add_argument("--commits", type=int, default=25)
    args = parser.parse_args()
    if not args.mssql_dsn or not args.pg_dsn:
        sys.exit("both MSSQL_DUCKLAKE_TEST_DSN and MSSQL_DUCKLAKE_PG_DSN are needed (make bench sets them)")

    body = workload(args.rows, args.commits)
    load = (
        "LOAD 'build/release/extension/mssql/mssql.duckdb_extension';\n"
        "LOAD 'build/release/extension/mssql_ducklake/mssql_ducklake.duckdb_extension';\n"
    )
    # a fresh catalog per backend, so initialization is measured rather than inherited
    reset_mssql = (
        f"ATTACH '{args.mssql_dsn}' AS srv (TYPE mssql);\n"
        "SELECT mssql_exec('srv', 'DECLARE @s NVARCHAR(MAX)=N''''; "
        "SELECT @s += N''DROP TABLE ''+QUOTENAME(s.name)+N''.''+QUOTENAME(t.name)+N'';'' "
        "FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id "
        "WHERE t.name LIKE ''ducklake%''; EXEC sp_executesql @s;');\n"
        "DETACH srv;\n"
    )
    reset_pg = (
        "INSTALL postgres; LOAD postgres;\n"
        f"ATTACH '{args.pg_dsn}' AS pg (TYPE postgres);\n"
        # the schema has to be named through the attached catalog, so switch into it first - the
        # same dance ducklake's own postgres test config does
        "USE pg;\n"
        "DROP SCHEMA public CASCADE;\n"
        "CREATE SCHEMA public;\n"
        "USE memory;\n"
        "DETACH pg;\n"
    )

    results = {}
    for name, reset, attach in (
        ("mssql", reset_mssql, f"ATTACH 'ducklake:mssql:{args.mssql_dsn}' AS lake (DATA_PATH '{args.data_path}/mssql');"),
        ("postgres", reset_pg, f"ATTACH 'ducklake:postgres:{args.pg_dsn}' AS lake (DATA_PATH '{args.data_path}/postgres');"),
    ):
        script = load + reset + body.replace("{ATTACH}", attach)
        print(f"running {name} ...", file=sys.stderr)
        results[name] = run(args.duckdb, script)

    phases = [p for p in results["mssql"] if p in results["postgres"]]
    width = max(len(p) for p in phases)
    print(f"\n{'phase'.ljust(width)}  {'mssql':>9}  {'postgres':>9}  ratio")
    print("-" * (width + 32))
    totals = {"mssql": 0.0, "postgres": 0.0}
    for phase in phases:
        mssql, postgres = results["mssql"][phase], results["postgres"][phase]
        totals["mssql"] += mssql
        totals["postgres"] += postgres
        ratio = mssql / postgres if postgres else float("inf")
        print(f"{phase.ljust(width)}  {mssql:9.3f}  {postgres:9.3f}  {ratio:5.2f}x")
    ratio = totals["mssql"] / totals["postgres"] if totals["postgres"] else float("inf")
    print("-" * (width + 32))
    print(f"{'total'.ljust(width)}  {totals['mssql']:9.3f}  {totals['postgres']:9.3f}  {ratio:5.2f}x")
    print("\nratio < 1 means the SQL Server backend is faster; the target is 'not worse' (specs/004).")


if __name__ == "__main__":
    main()
