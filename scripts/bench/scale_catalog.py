#!/usr/bin/env python3
"""A production-shaped DuckLake catalog, and what it costs on each backend.

`compare_backends.py` runs one table through a short workload, which measures the per-statement
overhead well and says nothing about how either backend ages. Production is a catalog with hundreds
or thousands of tables and thousands of snapshots behind them, where `ducklake_snapshot`,
`ducklake_data_file` and `ducklake_file_column_stats` have grown to the point where plans and key
widths start to decide the answer. That is what this builds, and then measures against.

The maintenance functions are the other half. They are the operations that read the WHOLE catalog
rather than one table's slice of it - expiring snapshots, merging adjacent files, cleaning up
files - so they are where catalog size shows up first, and they are exactly what a lake runs on a
schedule and nobody watches.

    make bench-scale                                  # the default shape, a few minutes
    make bench-scale BENCH_SCALE_ARGS='--tables 1000' # the production shape, considerably longer

Both servers must be up: `make docker-up && make bench-up`.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

PHASE = re.compile(r"^phase:(?P<name>[a-z_]+)$")
TIMING = re.compile(r"Run Time \(s\): real (?P<real>[0-9.]+)")

# The order they are reported in, which is the order they happen.
PHASES = [
    "create_schemas",
    "create_tables",
    "first_commits",
    "second_commits",
    "reattach",
    "list_snapshots",
    "table_info",
    "filtered_read",
    "flush_inlined",
    "expire_snapshots",
    "merge_adjacent",
    "cleanup_files",
    "checkpoint",
    "deep_history",
    "deep_list_snapshots",
    "deep_read_latest",
    "deep_read_filtered",
    "deep_time_travel_old",
    "deep_time_travel_mid",
    "deep_table_changes",
    "deep_reattach",
    "evolution",
    "evolution_read_latest",
    "evolution_time_travel",
    "evolution_table_info",
]

LOAD = (
    "LOAD 'build/release/extension/mssql/mssql.duckdb_extension';\n"
    "LOAD 'build/release/extension/mssql_ducklake/mssql_ducklake.duckdb_extension';\n"
)


def reset_sql(backend: str, mssql_dsn: str, pg_dsn: str) -> str:
    """Drop whatever the last run left, so the build below starts from nothing."""
    if backend == "postgres":
        return (
            "INSTALL postgres; LOAD postgres;\n"
            f"ATTACH '{pg_dsn}' AS pg (TYPE postgres);\n"
            "USE pg;\nDROP SCHEMA public CASCADE;\nCREATE SCHEMA public;\nUSE memory;\nDETACH pg;\n"
        )
    return (
        f"ATTACH '{mssql_dsn}' AS srv (TYPE mssql);\n"
        "SELECT mssql_exec('srv', 'DECLARE @s NVARCHAR(MAX)=N''''; "
        "SELECT @s += N''DROP TABLE ''+QUOTENAME(s.name)+N''.''+QUOTENAME(t.name)+N'';'' "
        "FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id "
        "WHERE t.name LIKE ''ducklake%''; EXEC sp_executesql @s;');\n"
        "DETACH srv;\n"
    )


def warmup_sql(backend: str, mssql_dsn: str, pg_dsn: str) -> str:
    """Open the backend and leave it attached, before anything is timed.

    A connection pool does not outlive the process that made it, and an mssql attach spends its
    time building one - three TDS logins. Detaching the warm catalog would throw that away, so it
    stays attached for the whole run. Postgres gets the same treatment; a comparison where one side pays connection setup at
    measurement time and the other does not is not a comparison.
    """
    if backend == "postgres":
        return f"INSTALL postgres; LOAD postgres;\nATTACH '{pg_dsn}' AS warm (TYPE postgres);\nSELECT 1;\n"
    return (
        f"ATTACH '{mssql_dsn}' AS warm (TYPE mssql);\n"
        "SELECT count(*) FROM mssql_scan('warm', 'SELECT 1 AS x');\n"
    )


def attach_sql(backend: str, mssql_dsn: str, pg_dsn: str, data_path: str) -> str:
    if backend == "postgres":
        return f"ATTACH 'ducklake:postgres:{pg_dsn}' AS lake (DATA_PATH '{data_path}');"
    return f"ATTACH 'ducklake:mssql:{mssql_dsn}' AS lake (DATA_PATH '{data_path}');"


def build_and_measure_sql(tables: int, rows: int, schemas: int, columns: int, deep_tables: int,
                          deep_snapshots: int, evolutions: int, attach: str) -> str:
    """The catalog, then the questions asked of it.

    Every CREATE TABLE and every INSERT is its own DuckLake commit, so `tables` tables with two
    rounds of inserts leave roughly `3 * tables` snapshots behind - which is the point: the
    maintenance functions below then have a real catalog to walk.

    The inserts are deliberately of two shapes. The first round is small enough to be inlined into
    the catalog, the second writes a parquet file, so the catalog ends up holding both kinds and
    `ducklake_flush_inlined_data` has something to do.
    """
    # Spread over schemas, because a real lake is not one flat namespace and the catalog's schema
    # tables are joined on every catalog load. Round-robin rather than in blocks, so no schema is
    # the one that happens to hold everything touched last.
    def qualified(i: int) -> str:
        return f"lake.s{i % schemas}.t{i}"

    # Wide, because width is what actually grows the catalog: ducklake_column holds a row per
    # column per table, and ducklake_file_column_stats a row per column per FILE. A 40-column table
    # writes forty stats rows for every file it commits, so a narrow table understates the catalog
    # by more than an order of magnitude. The types are mixed on purpose - the stats rows differ by
    # type, and VARCHAR min/max are the ones stored as text with the BIN2 collation.
    def column_defs() -> str:
        out = []
        for c in range(columns):
            kind = ("BIGINT", "VARCHAR", "DECIMAL(18, 4)", "DATE", "DOUBLE")[c % 5]
            out.append(f"c{c} {kind}")
        return ", ".join(out)

    def column_values(seed: str) -> str:
        out = []
        for c in range(columns):
            out.append(("{s}", "'v' || ({s})::VARCHAR", "({s} * 1.5)::DECIMAL(18, 4)",
                        "DATE '2020-01-01' + ({s})::INTEGER", "({s} * 0.25)::DOUBLE")[c % 5].format(s=seed))
        return ", ".join(out)

    schema_ddl = "\n".join(f"CREATE SCHEMA lake.s{i};" for i in range(schemas))
    creates = "\n".join(f"CREATE TABLE {qualified(i)}(id BIGINT, {column_defs()});" for i in range(tables))
    first = "\n".join(f"INSERT INTO {qualified(i)} SELECT {i}, {column_values(str(i))};"
                      for i in range(tables))
    second = "\n".join(
        f"INSERT INTO {qualified(i)} SELECT r, {column_values('r')} FROM range({rows}) t(r);"
        for i in range(tables)
    )
    # a table in the middle, so the read is not answered by whatever was touched last
    probe = qualified(tables // 2)

    # Depth, not just breadth. A lake that has been running writes into the same tables over and
    # over, so a few tables carry thousands of snapshots and thousands of small files between them -
    # which is the state compaction exists for, and the state a read has to plan against. Each of
    # these commits writes a file rather than inlining, because it is the data files a read has to
    # prune, and an inlined commit leaves none.
    deep = [qualified(i) for i in range(min(deep_tables, tables))]
    deep_writes = "\n".join(
        f"INSERT INTO {t} SELECT {n} * 100 + r, {column_values(f'({n} * 100 + r)')} FROM range(20) t(r);"
        for n in range(deep_snapshots) for t in deep
    )
    deep_probe = deep[0] if deep else probe

    # The versions to travel to are READ from the history rather than computed. The maintenance
    # functions above commit as they see fit, so counting the statements we emit does not give the
    # snapshot ids they land on - an earlier cut of this asked for version 52 of a table that did
    # not exist until 61. A marker before each block records where it started, and the targets are
    # offsets from that.
    deep_commits = deep_tables * deep_snapshots
    mark_deep_base = "SET VARIABLE deep_base = (SELECT max(snapshot_id) FROM ducklake_snapshots('lake'));"
    pick_deep = (
        f"SET VARIABLE deep_old = getvariable('deep_base') + {max(1, deep_commits // 10)};\n"
        f"SET VARIABLE deep_mid = getvariable('deep_base') + {max(2, deep_commits // 2)};"
    )
    mark_ev_base = "SET VARIABLE ev_base = (SELECT max(snapshot_id) FROM ducklake_snapshots('lake'));"
    pick_ev = f"SET VARIABLE ev_old = getvariable('ev_base') + {max(2, evolutions // 5)};"

    # One table that keeps changing shape. Schema evolution is its own kind of load on the catalog:
    # ducklake_column gains a row per column per VERSION rather than per column, ducklake_schema_versions
    # grows with every change, and a read at an old snapshot has to resolve the columns as they were
    # then rather than as they are now. A lake that has been in production has done this many times.
    evolution_sql = ["CREATE TABLE lake.s0.ev(id BIGINT, c0 BIGINT);"]
    live: list = []  # the added columns still present, under their current names
    for i in range(evolutions):
        evolution_sql.append(f"ALTER TABLE lake.s0.ev ADD COLUMN e{i} BIGINT;")
        live.append(f"e{i}")
        # written through an explicit column list, because the shape under it keeps moving
        evolution_sql.append(f"INSERT INTO lake.s0.ev (id, c0) VALUES ({i}, {i});")
        if i % 3 == 2 and len(live) > 1:
            # the oldest goes, so the table keeps a moving window rather than growing without end
            evolution_sql.append(f"ALTER TABLE lake.s0.ev DROP COLUMN {live.pop(0)};")
        if i % 5 == 4 and live:
            # by current name: a column renamed here must not be dropped by its old one later
            evolution_sql.append(f"ALTER TABLE lake.s0.ev RENAME COLUMN {live[-1]} TO r{i};")
            live[-1] = f"r{i}"
    evolution_ddl = "\n".join(evolution_sql)
    return f"""
SELECT 'phase:create_schemas';
{schema_ddl}
SELECT 'phase:create_tables';
{creates}
SELECT 'phase:first_commits';
{first}
SELECT 'phase:second_commits';
{second}
SELECT 'phase:reattach';
DETACH lake;
{attach}
SELECT 'phase:list_snapshots';
SELECT count(*) FROM ducklake_snapshots('lake');
SELECT 'phase:table_info';
SELECT count(*) FROM ducklake_table_info('lake');
SELECT 'phase:filtered_read';
SELECT count(*), sum(c0) FROM {probe} WHERE id BETWEEN 10 AND 40;
SELECT 'phase:flush_inlined';
SELECT count(*) FROM ducklake_flush_inlined_data('lake');
SELECT 'phase:expire_snapshots';
SELECT count(*) FROM ducklake_expire_snapshots('lake', dry_run => true);
SELECT 'phase:merge_adjacent';
SELECT count(*) FROM ducklake_merge_adjacent_files('lake');
SELECT 'phase:cleanup_files';
SELECT count(*) FROM ducklake_cleanup_old_files('lake', dry_run => true, cleanup_all => true);
SELECT 'phase:checkpoint';
CHECKPOINT;
{mark_deep_base}
SELECT 'phase:deep_history';
{deep_writes}
{pick_deep}
SELECT 'phase:deep_list_snapshots';
SELECT count(*) FROM ducklake_snapshots('lake');
SELECT 'phase:deep_read_latest';
SELECT count(*), sum(c0) FROM {deep_probe};
SELECT 'phase:deep_read_filtered';
SELECT count(*), sum(c0) FROM {deep_probe} WHERE id BETWEEN 5 AND 9;
SELECT 'phase:deep_time_travel_old';
SELECT count(*) FROM {deep_probe} AT (VERSION => getvariable('deep_old'));
SELECT 'phase:deep_time_travel_mid';
SELECT count(*) FROM {deep_probe} AT (VERSION => getvariable('deep_mid'));
SELECT 'phase:deep_table_changes';
SELECT count(*) FROM ducklake_table_changes('lake', 's0', 't0', getvariable('deep_old'), getvariable('deep_mid'));
SELECT 'phase:deep_reattach';
DETACH lake;
{attach}
SELECT count(*) FROM {deep_probe};
{mark_ev_base}
SELECT 'phase:evolution';
{evolution_ddl}
{pick_ev}
SELECT 'phase:evolution_read_latest';
SELECT count(*), sum(c0) FROM lake.s0.ev;
SELECT 'phase:evolution_time_travel';
SELECT count(*) FROM lake.s0.ev AT (VERSION => getvariable('ev_old'));
SELECT 'phase:evolution_table_info';
SELECT count(*) FROM ducklake_table_info('lake');
SELECT 'phase:end';
SELECT count(*) FROM {probe};
"""


def run(duckdb: str, script: str, expected_rows: int) -> dict:
    """One backend's whole run. Verifies the result before returning any timing.

    The check is not ceremony: an attach that fails costs almost nothing and would read as the best
    result in the table. That mistake has been made here twice.
    """
    proc = subprocess.run(
        [duckdb, "-unsigned", "-csv", "-noheader", "-cmd", ".timer on"],
        input=script, capture_output=True, text=True, timeout=14400,
    )
    # `.timer on` prints its line AFTER the result, so the last line of stdout is a timing, not the
    # answer. Take the last line that is neither a timing nor a phase marker.
    values = [ln.strip() for ln in proc.stdout.splitlines()
              if ln.strip() and not TIMING.search(ln) and not PHASE.match(ln.strip())]
    tail = values[-1] if values else ""
    if proc.returncode != 0 or tail != str(expected_rows):
        errors = [ln for ln in (proc.stdout + proc.stderr).splitlines() if "Error" in ln]
        sys.exit(f"run failed (last line {tail!r}, wanted {expected_rows}):\n" +
                 "\n".join(errors[:5]) + f"\n{proc.stdout[-1500:]}")
    phases, current = {}, None
    for line in proc.stdout.splitlines():
        marker = PHASE.match(line.strip())
        if marker:
            current = marker.group("name")
            continue
        timing = TIMING.search(line)
        if timing and current and current != "end":
            phases[current] = phases.get(current, 0.0) + float(timing.group("real"))
    return phases


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--mssql-dsn", default=os.environ.get("MSSQL_DUCKLAKE_TEST_DSN", ""))
    parser.add_argument("--pg-dsn", default=os.environ.get("MSSQL_DUCKLAKE_PG_DSN", ""))
    parser.add_argument("--tables", type=int, default=200,
                        help="tables in the catalog; 1000 is the production shape")
    parser.add_argument("--rows", type=int, default=200, help="rows in each table's file-backed insert")
    parser.add_argument("--schemas", type=int, default=10, help="schemas the tables are spread over")
    parser.add_argument("--columns", type=int, default=40,
                        help="columns per table beyond the id; width is what grows the catalog")
    parser.add_argument("--deep-tables", type=int, default=3,
                        help="tables given a deep snapshot history, so reads can be measured against one")
    parser.add_argument("--deep-snapshots", type=int, default=1000,
                        help="file-backed commits into EACH deep table; this is what makes reads interesting")
    parser.add_argument("--evolutions", type=int, default=100,
                        help="schema changes on one table, each followed by a commit")
    parser.add_argument("--backends", default="mssql,postgres", help="comma-separated: mssql, postgres")
    args = parser.parse_args()

    backends = [b.strip() for b in args.backends.split(",") if b.strip()]
    unknown = [b for b in backends if b not in ("mssql", "postgres")]
    if unknown:
        sys.exit(f"unknown backend(s): {', '.join(unknown)}")
    if "mssql" in backends and not args.mssql_dsn:
        sys.exit("MSSQL_DUCKLAKE_TEST_DSN is needed (make bench-scale sets it)")
    if "postgres" in backends and not args.pg_dsn:
        sys.exit("MSSQL_DUCKLAKE_PG_DSN is needed (make bench-scale sets it)")

    results = {}
    for backend in backends:
        data_path = tempfile.mkdtemp(prefix=f"ducklake_scale_{backend}_")
        attach = attach_sql(backend, args.mssql_dsn, args.pg_dsn, data_path)
        script = (LOAD
                  + reset_sql(backend, args.mssql_dsn, args.pg_dsn)
                  + warmup_sql(backend, args.mssql_dsn, args.pg_dsn)
                  + attach + "\n"
                  + build_and_measure_sql(args.tables, args.rows, args.schemas, args.columns,
                                          args.deep_tables, args.deep_snapshots, args.evolutions, attach))
        print(f"building {args.tables} tables on {backend} ...", file=sys.stderr)
        # the probe table holds one inlined row plus the file-backed insert
        results[backend] = run(args.duckdb, script, args.rows + 1)

    present = [p for p in PHASES if all(p in results[b] for b in backends)]
    width = max(len(p) for p in present)
    header = f"{'phase'.ljust(width)}  " + "  ".join(b.rjust(10) for b in backends)
    if len(backends) == 2:
        header += f"  {'ratio':>8}"
    stats_rows = args.tables * (args.columns + 1)
    print(f"\n{args.tables} tables over {args.schemas} schemas, {args.columns + 1} columns each, "
          f"~{3 * args.tables + args.schemas} snapshots,\n{args.rows} rows per file-backed insert, so "
          f"~{stats_rows} rows in ducklake_column and ~{stats_rows} per round in file column stats.\n"
          f"{args.deep_tables} tables then take {args.deep_snapshots} file-backed commits each, so the reads "
          f"below run against\n~{args.deep_tables * args.deep_snapshots} data files and a history that deep.\n"
          f"One table then takes {args.evolutions} schema changes, so a read at an old version has to "
          f"rebuild the schema as it was")
    print(f"\n{header}")
    print("-" * len(header))
    totals = {b: 0.0 for b in backends}
    for phase in present:
        row = f"{phase.ljust(width)}  " + "  ".join(f"{results[b][phase]:10.3f}" for b in backends)
        if len(backends) == 2:
            a, b = results[backends[0]][phase], results[backends[1]][phase]
            row += f"  {a / b:7.2f}x" if b else f"  {'-':>8}"
        for b in backends:
            totals[b] += results[b][phase]
        print(row)
    print("-" * len(header))
    row = f"{'total'.ljust(width)}  " + "  ".join(f"{totals[b]:10.3f}" for b in backends)
    if len(backends) == 2 and totals[backends[1]]:
        row += f"  {totals[backends[0]] / totals[backends[1]]:7.2f}x"
    print(row)
    print("\nThe maintenance phases walk the whole catalog rather than one table's slice of it,")
    print("which is where catalog size shows up first. expire_snapshots and cleanup_files run as")
    print("dry runs, so they measure the reads without destroying the catalog under the next phase.")


if __name__ == "__main__":
    main()
