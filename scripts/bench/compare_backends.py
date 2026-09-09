#!/usr/bin/env python3
"""Compare DuckLake metadata backends, and the two commit paths, on the same workload.

specs/004 states the performance target as "not worse than the postgres backend", which until now
was an assertion. One binary drives every arm: this extension embeds ducklake, so `ducklake:mssql:`
uses the manager in this repository and `ducklake:postgres:` uses the in-tree one, in the same
process against the same DuckDB.

Three arms, selectable with --arms:

    mssql         phase 1 - DuckLake's own commit loop, statement by statement
    mssql-fast    phase 2 - the same, with the server-side apply armed (specs/005)
    postgres      the reference the target is stated against

The two mssql arms differ only by MSSQL_DUCKLAKE_SERVER_COMMIT in the environment, so the
difference between them is the commit path and nothing else.

Every catalog is dropped and rebuilt per arm, so the numbers include initialization - which is
where this manager does the most extra work (its keys, indexes and collations).

    make bench                 # all three arms: make docker-up && make bench-up first
    make bench-paths           # the two commit paths only; no postgres needed
"""

import argparse
import os
import re
import subprocess
import sys

PHASE = re.compile(r"^phase:(?P<name>[a-z_]+)$")
TIMING = re.compile(r"Run Time \(s\): real (?P<real>[0-9.]+)")


def workload(rows: int, commits: int, file_rows: int, partitions: int) -> str:
    """The statements, with a marker before each phase. Metadata cost dominates the small commits;
    the bulk insert and the filtered reads are where the data path and the indexes show up.

    `file_commits` exists for the comparison of commit paths. A single-row INSERT is inlined into
    the catalog, and phase 2 does not apply an inlined commit - it stages, sees a shape it does not
    cover, and hands back to the commit loop, so those phases can only show phase 2's overhead. A
    commit past the inlining limit writes a parquet file, which is the shape the server-side apply
    does cover. Both are worth measuring, and confusing them is how one would conclude the wrong
    thing about either.

    `wide_commit` is the regime the server-side apply was designed for: ONE commit writing hundreds
    of data files, where a single bulk load replaces hundreds of INSERT statements. If phase 2 does
    not win here it does not win anywhere."""
    small = "\n".join(f"INSERT INTO lake.events VALUES ({i}, 'row {i}', {i} * 1.5);" for i in range(commits))
    later = "\n".join(f"INSERT INTO lake.events VALUES ({1000 + i}, 'late {i}', {i});" for i in range(commits))
    files = "\n".join(
        f"INSERT INTO lake.events SELECT {2000 + i} * 1000 + r, 'file {i}', r FROM range({file_rows}) t(r);"
        for i in range(commits)
    )
    return f"""
SELECT 'phase:attach';
{{ATTACH}}
SELECT 'phase:create_table';
CREATE TABLE lake.events(id BIGINT, name VARCHAR, amount DECIMAL(18, 4));
SELECT 'phase:small_commits';
{small}
SELECT 'phase:file_commits';
{files}
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
SELECT 'phase:wide_setup';
CREATE TABLE lake.wide(part BIGINT, v BIGINT);
ALTER TABLE lake.wide SET PARTITIONED BY (part);
SELECT 'phase:wide_commit';
INSERT INTO lake.wide SELECT r % {partitions}, r FROM range({partitions} * 20) t(r);
SELECT 'phase:snapshots';
SELECT count(*) FROM ducklake_snapshots('lake');
SELECT 'phase:reattach';
DETACH lake;
{{ATTACH}}
SELECT count(*) FROM lake.events;
SELECT 'phase:end';
"""


def run(duckdb: str, script: str, env: dict) -> dict:
    """Run one arm's script, returning seconds per phase."""
    proc = subprocess.run(
        [duckdb, "-unsigned", "-csv", "-noheader", "-cmd", ".timer on"],
        input=script, capture_output=True, text=True, timeout=3600,
        env={**os.environ, **env},
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
    parser.add_argument("--file-commit-rows", type=int, default=100,
                        help="rows per commit in the file_commits phase; must exceed the inlining limit")
    parser.add_argument("--arms", default="mssql,mssql-fast,postgres",
                        help="comma-separated: mssql, mssql-fast, mssql-fast-nofetch, postgres")
    parser.add_argument("--partitions", type=int, default=200,
                        help="partitions in the wide_commit phase - one commit writing that many data files")
    parser.add_argument("--repeat", type=int, default=3,
                        help="rounds over the arms; the reported time is the fastest round per phase")
    args = parser.parse_args()
    arms = [a.strip() for a in args.arms.split(",") if a.strip()]
    unknown = [a for a in arms if a not in ("mssql", "mssql-fast", "mssql-fast-nofetch", "postgres")]
    if unknown:
        sys.exit(f"unknown arm(s): {', '.join(unknown)}")
    if any(a.startswith("mssql") for a in arms) and not args.mssql_dsn:
        sys.exit("MSSQL_DUCKLAKE_TEST_DSN is needed for the mssql arms (make bench sets it)")
    if "postgres" in arms and not args.pg_dsn:
        sys.exit("MSSQL_DUCKLAKE_PG_DSN is needed for the postgres arm (make bench sets it)")

    body = workload(args.rows, args.commits, args.file_commit_rows, args.partitions)
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

    mssql_attach = f"ATTACH 'ducklake:mssql:{args.mssql_dsn}' AS lake (DATA_PATH '{args.data_path}/mssql');"
    # the two mssql arms differ ONLY by this variable, which is what makes the comparison a
    # comparison of commit paths rather than of two runs that happen to differ somewhere
    spec = {
        "mssql": (reset_mssql, mssql_attach, {}),
        "mssql-fast": (reset_mssql, mssql_attach, {"MSSQL_DUCKLAKE_SERVER_COMMIT": "1"}),
        "mssql-fast-nofetch": (reset_mssql, mssql_attach,
                               {"MSSQL_DUCKLAKE_SERVER_COMMIT": "1", "MSSQL_DUCKLAKE_SERVER_COMMIT_SKIP_FETCH": "1"}),
        "postgres": (reset_pg,
                     f"ATTACH 'ducklake:postgres:{args.pg_dsn}' AS lake (DATA_PATH '{args.data_path}/postgres');",
                     {}),
    }

    # Rounds, not one run per arm. Whichever arm goes first pays for a cold server - plan cache,
    # buffer pool, the extension's own catalog cache - and the phases that cannot depend on the
    # commit path at all (attach, snapshots) showed that bias plainly when this ran once per arm.
    # Alternating the order and keeping the fastest round per phase takes it back out.
    results = {a: {} for a in arms}
    for round_no in range(args.repeat):
        order = arms if round_no % 2 == 0 else list(reversed(arms))
        for name in order:
            reset, attach, env = spec[name]
            script = load + reset + body.replace("{ATTACH}", attach)
            print(f"round {round_no + 1}/{args.repeat}: running {name} ...", file=sys.stderr)
            timings = run(args.duckdb, script, env)
            for phase, seconds in timings.items():
                best = results[name].get(phase)
                results[name][phase] = seconds if best is None else min(best, seconds)

    phases = [p for p in results[arms[0]] if all(p in results[a] for a in arms)]
    width = max(len(p) for p in phases)
    header = f"{'phase'.ljust(width)}  " + "  ".join(a.rjust(10) for a in arms)
    ratios = []
    if "mssql" in arms and "mssql-fast" in arms:
        ratios.append(("fast/phase1", "mssql-fast", "mssql"))
    if "mssql" in arms and "mssql-fast-nofetch" in arms:
        ratios.append(("nofetch/ph1", "mssql-fast-nofetch", "mssql"))
    if "mssql" in arms and "postgres" in arms:
        ratios.append(("mssql/pg", "mssql", "postgres"))
    if "mssql-fast" in arms and "postgres" in arms:
        ratios.append(("fast/pg", "mssql-fast", "postgres"))
    header += "  " + "  ".join(label.rjust(11) for label, _, _ in ratios)
    print("\n" + header)
    print("-" * len(header))
    totals = {a: 0.0 for a in arms}
    for phase in phases:
        row = f"{phase.ljust(width)}  " + "  ".join(f"{results[a][phase]:10.3f}" for a in arms)
        for _, num, den in ratios:
            d = results[den][phase]
            row += f"  {results[num][phase] / d:10.2f}x" if d else f"  {'-':>11}"
        for a in arms:
            totals[a] += results[a][phase]
        print(row)
    print("-" * len(header))
    row = f"{'total'.ljust(width)}  " + "  ".join(f"{totals[a]:10.3f}" for a in arms)
    for _, num, den in ratios:
        row += f"  {totals[num] / totals[den]:10.2f}x" if totals[den] else f"  {'-':>11}"
    print(row)
    print(f"\nFastest of {args.repeat} rounds per phase, arms alternated each round.")
    print("Ratios below 1.00 favour the numerator. The target of specs/004 is mssql/pg not worse")
    print("than 1.00; specs/005 is about fast/phase1, and only file_commits and bulk_insert are")
    print("commits the server-side apply accepts - the rest measure what its staging costs.")


if __name__ == "__main__":
    main()
