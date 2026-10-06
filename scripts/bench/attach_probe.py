#!/usr/bin/env python3
"""One ATTACH and the catalog load behind it, statement by statement.

The pushdown measurement (design/004, design/005) left one question open: attaching a large catalog
costs 1.6-1.8x with the mssql extension's remote pushdown on, with the SAME number of connection
acquires - so the cost is in the planner, not in round trips, and what matters next is WHICH
statements pay it. This runs the attach of an existing catalog with DuckDB's QueryLog on, which
records every statement DuckLake issues internally, normalises the literals out of each one and
prints the shapes by total time, per arm.

    make bench-attach-probe                       # both arms, two rounds, shapes by total time
    make bench-attach-probe PROBE_ARGS='--full'   # and the full text of every shape

The connection string is read from the environment (the Makefile target exports it) and never
printed: every statement is redacted before it is shown, because the ATTACH itself carries the DSN.
"""

import argparse
import collections
import csv
import os
import re
import subprocess
import sys
import tempfile

ARMS = ("true", "false")

# the bench catalog's schema (scale_catalog.py builds it there; specs/015 R6)
BENCH_SCHEMA = os.environ.get("MSSQL_DUCKLAKE_BENCH_SCHEMA", "bench").strip() or "bench"

# a logged statement can be enormous - the sweep below builds plans of a thousand branches, and the
# log holds their text
csv.field_size_limit(1 << 28)


def probe_sql(duckdb_build: str, dsn: str, arm: str, out_csv: str, read_only: bool = False) -> str:
    """The attach, and the log of what it ran.

    `storage = 'memory'` puts the entries in duckdb_logs with microsecond timestamps; the CLI's own
    log storage prints them with a hundredth-of-a-second clock, which is coarser than some of the
    statements. No DATA_PATH: the catalog carries its own, and this never creates one.
    """
    return f"""
SET autoload_known_extensions = false;
SET autoinstall_known_extensions = false;
LOAD '{duckdb_build}/extension/mssql/mssql.duckdb_extension';
LOAD '{duckdb_build}/extension/mssql_ducklake/mssql_ducklake.duckdb_extension';
CALL enable_logging('QueryLog', storage = 'memory');
ATTACH 'ducklake:mssql:{dsn}' AS lake (METADATA_SCHEMA '{BENCH_SCHEMA}', METADATA_PARAMETERS MAP {{'remote_pushdown': '{arm}'}}{', READ_ONLY' if read_only else ''});
SELECT count(*) AS tables FROM duckdb_tables() WHERE database_name = 'lake';
CALL disable_logging();
COPY (SELECT epoch_ms(timestamp) AS ms, message FROM duckdb_logs WHERE type = 'QueryLog' ORDER BY timestamp)
  TO '{out_csv}' (FORMAT csv, HEADER);
"""


def union_sql(branches: int, columns: int = 1) -> str:
    """A plan of N remote scans under one set operation, the shape DuckLake's inlined-table probe has.

    `MigrateInlinedColumnNames` asks whether a catalog is already renamed with one statement per
    attach: `(SELECT … FROM <inlined table> LIMIT 0) UNION ALL (…)` over EVERY inlined table in the
    catalog. On a thousand-table lake that is a thousand branches in one plan - which is the only
    statement of an attach whose plan grows with the catalog, and the one shape a three-table
    imitation cannot produce. `LIMIT 0` means no row is ever read, so what this measures is purely
    planning. The columns are the ones every catalog table has, so it needs no inlined table to
    exist.
    """
    cols = ", ".join(["table_id", "table_uuid", "table_name", "schema_id"][:columns])
    branch = f"(SELECT {cols} FROM \"__ducklake_metadata_lake\".\"{BENCH_SCHEMA}\".ducklake_table LIMIT 0)"
    return " UNION ALL ".join([branch] * branches)


def refill_inlined(duckdb_bin: str, duckdb_build: str, dsn: str, env: dict, tables: int, schemas: int) -> str:
    """Put an inlined table back under every lake table: one small insert each, one commit each.

    The state the pushdown bench measured its reattach in, and the state the probe statement above
    is about - `flush_inlined` writes those rows to parquet and drops the inlined tables, so a
    catalog that has been through the full bench no longer has any, and an attach of it no longer
    issues the probe at all.
    """
    sql = [
        "SET autoload_known_extensions = false;",
        "SET autoinstall_known_extensions = false;",
        f"LOAD '{duckdb_build}/extension/mssql/mssql.duckdb_extension';",
        f"LOAD '{duckdb_build}/extension/mssql_ducklake/mssql_ducklake.duckdb_extension';",
        f"ATTACH 'ducklake:mssql:{dsn}' AS lake;",
    ]
    for i in range(tables):
        sql.append(f"INSERT INTO lake.s{i % schemas}.t{i} (id) VALUES ({i}), ({i + 1});")
    sql.append(f'SELECT count(*) AS inlined FROM "__ducklake_metadata_lake"."{BENCH_SCHEMA}".ducklake_inlined_data_tables;')
    proc = subprocess.run([duckdb_bin, "-unsigned", "-batch", "-no-agent"], input="\n".join(sql), env=env,
                          capture_output=True, text=True)
    out = redact(proc.stdout + proc.stderr, dsn)
    return "\n".join(line for line in out.splitlines() if line.strip())[-600:]


def run_arm(duckdb_bin: str, duckdb_build: str, dsn: str, arm: str, env: dict, read_only: bool = False,
            extra: str = "") -> list:
    """(duration_ms, statement) per logged statement, in order.

    A log entry is written when a statement STARTS - the ATTACH is logged before the statements it
    runs internally - so the time of a statement is the gap to the next entry. The last entry is
    `disable_logging`, which bounds the one before it.
    """
    with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as handle:
        out_csv = handle.name
    sql = probe_sql(duckdb_build, dsn, arm, out_csv, read_only)
    if extra:
        sql = sql.replace("CALL disable_logging();", extra + "\nCALL disable_logging();")
    proc = subprocess.run([duckdb_bin, "-unsigned", "-batch", "-no-agent"], input=sql, env=env,
                          capture_output=True, text=True)
    if proc.returncode != 0 or "Error" in proc.stderr:
        sys.stderr.write(redact(proc.stderr, dsn)[:4000])
    rows = []
    with open(out_csv, newline="") as fh:
        for row in csv.DictReader(fh):
            rows.append((int(row["ms"]), row["message"]))
    os.unlink(out_csv)
    timed = []
    for i, (ms, message) in enumerate(rows):
        nxt = rows[i + 1][0] if i + 1 < len(rows) else ms
        timed.append((nxt - ms, message))
    return timed


def redact(text: str, dsn: str) -> str:
    if dsn:
        text = text.replace(dsn, "<dsn>")
    return re.sub(r"(?i)password=[^;'\"]*", "Password=***", text)


def shape(statement: str) -> str:
    """The statement with its literals taken out, so repeats of one shape group together.

    `mssql_exec(ctx, sql)` and `mssql_scan(ctx, sql)` carry the whole T-SQL inside a literal, so the
    normalisation would collapse every one of them into one shape - and those are the manager\'s own
    round trips, the ones worth telling apart. A prefix of the inner statement is kept for them.
    """
    call = re.match(r"\s*(SELECT|FROM)?\s*\w*\s*(?:=\s*)?.*?\bmssql_(exec|scan)\('(?:[^']|'')*',\s*'((?:[^']|'')*)'",
                    statement, re.S)
    if call:
        inner = re.sub(r"\s+", " ", call.group(3)).strip()[:70]
        return f"mssql_{call.group(2)}: {inner}"
    out = re.sub(r"'(?:[^']|'')*'", "'?'", statement)
    out = re.sub(r"\b\d+\b", "?", out)
    # a VALUES list or an IN list of normalised literals is one shape whatever its length
    out = re.sub(r"(\((?:\?|'\?')(?:\s*,\s*(?:\?|'\?'))*\))(\s*,\s*\1)+", r"\1, ...", out)
    out = re.sub(r"\s+", " ", out).strip()
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--build", default="build/release", help="where the two loadables are")
    parser.add_argument("--mssql-dsn", default=os.environ.get("MSSQL_DUCKLAKE_TEST_DSN", ""))
    parser.add_argument("--rounds", type=int, default=2, help="rounds per arm, alternated")
    parser.add_argument("--top", type=int, default=8, help="shapes to print")
    parser.add_argument("--full", action="store_true", help="print each shape's full text")
    parser.add_argument("--read-only", action="store_true",
                        help="attach READ_ONLY, which skips DuckLake's migration re-run - the catalog load alone")
    parser.add_argument("--union-sweep", default="",
                        help="comma-separated branch counts: time one plan of N remote scans per arm")
    parser.add_argument("--union-columns", type=int, default=1, help="columns per branch (DuckLake's probe has 3)")
    parser.add_argument("--refill-inlined", type=int, default=0,
                        help="first put an inlined table back under this many lake tables (s<i %% schemas>.t<i>)")
    parser.add_argument("--refill-schemas", type=int, default=10, help="schemas those tables are spread over")
    parser.add_argument("--flush-tables", type=int, default=0,
                        help="N tables with inlined rows, one flush of them, broken down by statement")
    parser.add_argument("--insert-commits", type=int, default=0,
                        help="N inlined and N file-backed insert commits, broken down by statement")
    parser.add_argument("--create-tables", type=int, default=0,
                        help="N CREATE TABLE commits of the bench's width, broken down by statement")
    parser.add_argument("--shape-ab", type=int, default=0,
                        help="N calls of one statement through mssql_scan and mssql_scan_unsafe, in a transaction")
    parser.add_argument("--synth-tables", type=int, default=0,
                        help="make N plain remote tables and time one plan that scans all of them")
    parser.add_argument("--keep-synth", action="store_true", help="leave those tables behind")
    parser.add_argument("--union-distinct", default="",
                        help="comma-separated branch counts over DISTINCT inlined tables - DuckLake's real probe")
    parser.add_argument("--names-csv", default="/tmp/mssql_ducklake_inlined_names.csv")
    parser.add_argument("--legacy-migration", action="store_true",
                        help="time DuckLake's own migration statements on the attach path, per arm")
    parser.add_argument("--debug-counts", action="store_true",
                        help="also run the ON arm with MSSQL_DEBUG=2 and count the planner's lines")
    args = parser.parse_args()
    if not args.mssql_dsn:
        return "MSSQL_DUCKLAKE_TEST_DSN is needed (make bench-attach-probe sets it)"
    dsn, env = args.mssql_dsn, dict(os.environ)
    build = os.path.abspath(args.build)

    if args.flush_tables:
        # What the bench's flush_inlined phase is made of - 135-144 s for 1000 tables. N tables of the
        # bench's width in a schema of their own, one small (inlined) insert each, then ONE flush of
        # that schema; the flush's statements broken down by shape, per table.
        n = args.flush_tables
        kinds = ("BIGINT", "VARCHAR", "DECIMAL(18, 4)", "DATE", "DOUBLE")
        exprs = ("{s}", "'v' || ({s})::VARCHAR", "({s} * 1.5)::DECIMAL(18, 4)",
                 "DATE '2020-01-01' + ({s})::INTEGER", "({s} * 0.25)::DOUBLE")
        cols = ", ".join(f"c{c} {kinds[c % 5]}" for c in range(40))
        vals = ", ".join(exprs[c % 5].format(s="r") for c in range(40))
        body = "CREATE SCHEMA IF NOT EXISTS lake.probe_fl;\n"
        body += "".join(f"CREATE TABLE lake.probe_fl.t{i}(id BIGINT, {cols});\n" for i in range(n))
        body += "".join(f"INSERT INTO lake.probe_fl.t{i} SELECT r, {vals} FROM range(0, 2) t(r);\n" for i in range(n))
        body += "SELECT count(*) AS flushed FROM ducklake_flush_inlined_data('lake', schema_name := 'probe_fl');\n"
        body += "".join(f"DROP TABLE lake.probe_fl.t{i};\n" for i in range(n))
        timed = run_arm(args.duckdb, build, dsn, "false", env, False, body)
        start = next((i for i, (_, m) in enumerate(timed) if "ducklake_flush_inlined_data" in m), None)
        stop = next((i for i, (_, m) in enumerate(timed) if m.startswith("DROP TABLE lake.probe_fl")), len(timed))
        if start is None:
            print("the flush did not run - see the error above")
            return 0
        per = collections.defaultdict(lambda: [0, 0])
        for ms, m in timed[start:stop]:
            e = per[shape(redact(m, dsn))]
            e[0] += 1
            e[1] += ms
        if args.full:
            print("--- the flush, in order (first 45 statements) ---")
            for ms, m in timed[start:start + 45]:
                snap = re.search(r"WHERE (\d+) >= (?:tbl\.)?begin_snapshot", m) or re.search(
                    r"ducklake_snapshot VALUES \((\d+), [^,]+, (\d+)", m)
                tag = f"  [snapshot {snap.group(1)}{', schema ' + snap.group(2) if snap and snap.lastindex == 2 else ''}]" if snap else ""
                print(f"{ms:>6}  {shape(redact(m, dsn))[:100]}{tag}")
        total = sum(ms for ms, _ in timed[start:stop])
        print(f"flush of {n} tables: {total} ms, {total / n:.1f} ms a table, "
              f"{sum(c for c, _ in per.values()) / n:.1f} statements a table")
        print(f"{'n/table':>8} {'ms/table':>9}  shape")
        for shp, (count, ms) in sorted(per.items(), key=lambda kv: -kv[1][1])[: args.top]:
            print(f"{count / n:>8.1f} {ms / n:>9.1f}  {shp[:140]}")
        return 0

    if args.insert_commits:
        # What one data commit is made of: a table of the bench's width, then N inlined inserts
        # (2 rows) and N file-backed inserts (200 rows), each its own commit, broken down the same way.
        n = args.insert_commits
        kinds = ("BIGINT", "VARCHAR", "DECIMAL(18, 4)", "DATE", "DOUBLE")
        exprs = ("{s}", "'v' || ({s})::VARCHAR", "({s} * 1.5)::DECIMAL(18, 4)",
                 "DATE '2020-01-01' + ({s})::INTEGER", "({s} * 0.25)::DOUBLE")
        cols = ", ".join(f"c{c} {kinds[c % 5]}" for c in range(40))
        vals = ", ".join(exprs[c % 5].format(s="r") for c in range(40))
        body = ("CREATE SCHEMA IF NOT EXISTS lake.probe_ic;\n"
                f"CREATE TABLE lake.probe_ic.t(id BIGINT, {cols});\n")
        for i in range(n):
            body += f"INSERT INTO lake.probe_ic.t SELECT r, {vals} FROM range({i * 2}, {i * 2 + 2}) t(r);\n"
        for i in range(n):
            body += f"INSERT INTO lake.probe_ic.t SELECT r, {vals} FROM range({10000 + i * 200}, {10200 + i * 200}) t(r);\n"
        body += "DROP TABLE lake.probe_ic.t;\n"
        timed = run_arm(args.duckdb, build, dsn, "false", env, False, body)
        starts = [i for i, (_, m) in enumerate(timed) if m.startswith("INSERT INTO lake.probe_ic")]
        end = next((i for i, (_, m) in enumerate(timed) if m.startswith("DROP TABLE lake.probe_ic")), len(timed))
        for label, group in (("inlined (2 rows)", starts[:n]), ("file-backed (200 rows)", starts[n:])):
            per, user_ms = collections.defaultdict(lambda: [0, 0]), []
            for start in group:
                k = starts.index(start)
                stop = starts[k + 1] if k + 1 < len(starts) else end
                user_ms.append(sum(ms for ms, _ in timed[start:stop]))
                for ms, m in timed[start:stop]:
                    e = per[shape(redact(m, dsn))]
                    e[0] += 1
                    e[1] += ms
            print(f"\n=== {label}: {len(group)} commits, mean {sum(user_ms) / max(len(group), 1):.1f} ms, each {user_ms}")
            print(f"{'n/commit':>9} {'ms/commit':>10}  shape")
            for shp, (count, ms) in sorted(per.items(), key=lambda kv: -kv[1][1])[: args.top]:
                print(f"{count / len(group):>9.1f} {ms / len(group):>10.1f}  {shp[:140]}")
        return 0

    if args.create_tables:
        # What one CREATE TABLE commit is made of - the bench's create_tables phase, 169 ms a table on
        # the 1000-table catalog. Same width as the bench (id + 40 mixed columns), each its own
        # commit, in a schema of its own so nothing of the bench's is touched.
        n = args.create_tables
        kinds = ("BIGINT", "VARCHAR", "DECIMAL(18, 4)", "DATE", "DOUBLE")
        cols = ", ".join(f"c{c} {kinds[c % 5]}" for c in range(40))
        body = "CREATE SCHEMA IF NOT EXISTS lake.probe_ct;\n" + "".join(
            f"CREATE TABLE lake.probe_ct.t{i}(id BIGINT, {cols});\n" for i in range(n))
        body += "".join(f"DROP TABLE lake.probe_ct.t{i};\n" for i in range(n))
        timed = run_arm(args.duckdb, build, dsn, "false", env, False, body)
        creates = [i for i, (ms, m) in enumerate(timed) if m.startswith("CREATE TABLE lake.probe_ct")]
        if not creates:
            print("no CREATE TABLE ran - see the error above")
            return 0
        # every logged statement from one user CREATE to the next belongs to that commit
        per = collections.defaultdict(lambda: [0, 0])
        user_ms = []
        for k, start in enumerate(creates):
            stop = creates[k + 1] if k + 1 < len(creates) else next(
                (i for i, (_, m) in enumerate(timed) if m.startswith("DROP TABLE lake.probe_ct")), len(timed))
            user_ms.append(sum(ms for ms, _ in timed[start:stop]))
            for ms, m in timed[start:stop]:
                e = per[shape(redact(m, dsn))]
                e[0] += 1
                e[1] += ms
        if args.full:
            k = min(10, len(creates) - 1)
            start, stop = creates[k], creates[k + 1] if k + 1 < len(creates) else len(timed)
            print(f"--- commit #{k}, in order ---")
            for ms, m in timed[start:stop]:
                # the snapshot a catalog-load statement reads at, and the id a commit writes
                snap = re.search(r"WHERE (\d+) >= (?:tbl\.)?begin_snapshot", m) or re.search(
                    r"ducklake_snapshot VALUES \((\d+),", m)
                tag = f"  [snapshot {snap.group(1)}]" if snap else ""
                print(f"{ms:>6}  {shape(redact(m, dsn))[:110]}{tag}")
        print(f"{n} CREATE TABLE commits, ms each: {user_ms}")
        print(f"mean {sum(user_ms) / n:.1f} ms, statements per commit {sum(c for c, _ in per.values()) / n:.1f}")
        print(f"{'n/commit':>9} {'ms/commit':>10}  shape")
        for shp, (count, ms) in sorted(per.items(), key=lambda kv: -kv[1][1])[: args.top]:
            print(f"{count / n:>9.1f} {ms / n:>10.1f}  {shp[:150]}")
        return 0

    if args.shape_ab:
        # What a given shape saves per call: the same statement through mssql_scan (which describes
        # it first) and through mssql_scan_unsafe (which does not), N times in a row, inside a
        # transaction - which is where the describe takes the pinned connection.
        n = args.shape_ab
        cat = f'"__ducklake_metadata_lake"."{BENCH_SCHEMA}"'
        inner = ("SELECT TOP 1 snapshot_id, schema_version, next_catalog_id, next_file_id "
                 f"FROM {BENCH_SCHEMA}.ducklake_snapshot ORDER BY snapshot_id DESC")
        declared = ("columns := {'snapshot_id': 'BIGINT', 'schema_version': 'BIGINT', "
                    "'next_catalog_id': 'BIGINT', 'next_file_id': 'BIGINT'}")
        print(f"the manager's latest-snapshot statement, {n} calls in a transaction, ms")
        print(f"{'':>16} {'ON':>7} {'OFF':>7}")
        for fn, extra_arg in (("mssql_scan", ""), ("mssql_scan_unsafe", ", " + declared)):
            call = f"SELECT snapshot_id FROM {fn}('__ducklake_metadata_lake', '{inner}'{extra_arg})"
            body = "BEGIN TRANSACTION;\n" + "".join(f"{call};\n" for _ in range(n)) + "COMMIT;"
            got = {}
            for arm in ARMS:
                timed = run_arm(args.duckdb, build, dsn, arm, env, args.read_only, body)
                got[arm] = sum(ms for ms, m in timed if fn + "(" in m and "SELECT snapshot_id" in m)
            print(f"{fn:>16} {got['true']:>7} {got['false']:>7}")
        return 0

    if args.synth_tables:
        # The shape on its own, independent of any DuckLake state: N DISTINCT remote tables under one
        # UNION ALL, each read with LIMIT 0. That is DuckLake's inlined-table probe
        # (MigrateInlinedColumnNames) at catalog scale, and it is the one statement of an attach whose
        # plan grows with the catalog. Plain SQL Server tables in a schema of their own, made and
        # dropped here in one round trip each, so this needs no lake and no bench catalog.
        n = args.synth_tables
        cat, schema = '"__ducklake_metadata_lake"."attach_probe"', "attach_probe"
        cols = "_ducklake_row_id BIGINT, _ducklake_begin_snapshot BIGINT, _ducklake_end_snapshot BIGINT"
        make = (f"IF SCHEMA_ID(''{schema}'') IS NULL EXEC(''CREATE SCHEMA {schema}'');" +
                "".join(f"IF OBJECT_ID(''{schema}.probe_t{i}'') IS NULL CREATE TABLE {schema}.probe_t{i}({cols});"
                        for i in range(n)))
        drop = ("DECLARE @d NVARCHAR(MAX) = N''''; SELECT @d += N''DROP TABLE '' + QUOTENAME(s.name) + N''.'' "
                f"+ QUOTENAME(t.name) + N'';'' FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id "
                f"WHERE s.name = ''{schema}''; EXEC sp_executesql @d;")
        branches = " UNION ALL ".join(
            f'(SELECT _ducklake_row_id, _ducklake_begin_snapshot, _ducklake_end_snapshot FROM {cat}."probe_t{i}" '
            "LIMIT 0)" for i in range(n))
        setup = f"SELECT mssql_exec('lake', '{make}');"
        print(f"{n} distinct remote tables under one UNION ALL, LIMIT 0 - planning and whatever it executes")
        print(f"{'':>9} {'ON':>7} {'OFF':>7} {'ON/OFF':>7}")
        # the bare union is DuckLake's own statement; the count(*) wrapper is only there to make it
        # a statement with a result, and it is measured too in case the aggregate changes the plan
        statements = {"bare": f"{branches};", "count": f"SELECT count(*) FROM ({branches}) u;"}
        for label, stmt in statements.items():
            got = {}
            for arm in ARMS:
                timed = run_arm(args.duckdb, build, dsn, arm, env, args.read_only, setup + "\n" + stmt)
                got[arm] = max((ms for ms, m in timed if "UNION ALL" in m), default=0)
            ratio = got["true"] / got["false"] if got["false"] else 0
            print(f"{label:>9} {got['true']:>7} {got['false']:>7} {ratio:>7.2f}")
        if args.debug_counts:
            # what the ON arm actually sends for those branches, on the server's side of the wire
            log = ""
            for arm in ARMS:
                proc = subprocess.run([args.duckdb, "-unsigned", "-batch", "-no-agent"],
                                      input=probe_sql(build, dsn, arm, "/dev/null", args.read_only).replace(
                                          "CALL disable_logging();", setup + "\n" + f"{branches};\nCALL disable_logging();"),
                                      env=dict(env, MSSQL_DEBUG="2"), capture_output=True, text=True)
                out = proc.stdout + proc.stderr
                print(f"\n--- MSSQL_DEBUG=2, remote_pushdown={arm} ---")
                for needle in ("SELECT TOP (0)", "sp_describe_first_result_set", "sp_prepare", "SupportsPushdown(",
                               "RemoteExecute", "probe_t"):
                    print(f"{out.count(needle):>8}  {needle}")
        if not args.keep_synth:
            run_arm(args.duckdb, build, dsn, "false", env, False, f"SELECT mssql_exec('lake', '{drop}');")
            print(f"({n} probe tables dropped)")
        return 0

    if args.union_distinct:
        # The real shape, over DISTINCT tables. The synthetic sweep repeats one table, which lets the
        # extension bind it once; DuckLake's probe names every inlined table in the catalog, so the
        # plan has N different remote scans and N bindings - and that is the shape no small catalog
        # produces.
        cat = f'"__ducklake_metadata_lake"."{BENCH_SCHEMA}"'
        listing = run_arm(args.duckdb, build, dsn, "false", env, False,
                          f"COPY (SELECT table_name FROM {cat}.ducklake_inlined_data_tables ORDER BY table_id) "
                          f"TO '{args.names_csv}' (FORMAT csv);")
        names = [row.strip() for row in open(args.names_csv).read().splitlines() if row.strip()]
        print(f"inlined tables in the catalog: {len(names)}")
        print(f"{'branches':>9} {'ON':>7} {'OFF':>7} {'ON/OFF':>7}")
        for n in [int(x) for x in args.union_distinct.split(",")]:
            take = names[:n]
            if len(take) < n:
                print(f"{n:>9}  (only {len(take)} inlined tables exist)")
                continue
            branches = " UNION ALL ".join(
                f'(SELECT _ducklake_row_id, _ducklake_begin_snapshot, _ducklake_end_snapshot FROM {cat}."{t}" LIMIT 0)'
                for t in take)
            stmt = f"SELECT count(*) FROM ({branches}) u;"
            got = {}
            for arm in ARMS:
                timed = run_arm(args.duckdb, build, dsn, arm, env, args.read_only, stmt)
                got[arm] = max((ms for ms, m in timed if "UNION ALL" in m), default=0)
            ratio = got["true"] / got["false"] if got["false"] else 0
            print(f"{n:>9} {got['true']:>7} {got['false']:>7} {ratio:>7.2f}")
        return 0

    if args.legacy_migration:
        # What an attach paid before the manager took the migration over: DuckLake's own batch, as
        # ExecuteMigration sends it on the attach path (allow_failures, so the {IF_NOT_EXISTS}
        # placeholder is filled in - which the mssql extension's ALTER has no form for).
        cat = f'"__ducklake_metadata_lake"."{BENCH_SCHEMA}"'
        legacy = [
            f"ALTER TABLE {cat}.ducklake_data_file ADD COLUMN IF NOT EXISTS row_group_count BIGINT;",
            f"ALTER TABLE {cat}.ducklake_delete_file ADD COLUMN IF NOT EXISTS row_group_count BIGINT;",
            f"ALTER TABLE {cat}.ducklake_file_column_stats ADD COLUMN IF NOT EXISTS min_is_exact BOOLEAN DEFAULT NULL;",
            f"ALTER TABLE {cat}.ducklake_schema ADD COLUMN IF NOT EXISTS parent_schema_id BIGINT;",
            f"CREATE TABLE IF NOT EXISTS {cat}.ducklake_view_column_tag(view_id BIGINT, column_name VARCHAR, "
            "begin_snapshot BIGINT, end_snapshot BIGINT, key VARCHAR, value VARCHAR);",
            f"UPDATE {cat}.ducklake_metadata SET value = '1.1-dev1' WHERE key = 'version';",
        ]
        print("DuckLake's own migration statements on the attach path, ms per arm")
        print(f"{'ON':>7} {'OFF':>7}  statement")
        got = {}
        for arm in ARMS:
            got[arm] = run_arm(args.duckdb, build, dsn, arm, env, args.read_only, "\n".join(legacy))
        for i, stmt in enumerate(legacy):
            def find(arm, needle=stmt[:60]):
                return next((ms for ms, m in got[arm] if m.startswith(needle[:40])), 0)
            print(f"{find('true'):>7} {find('false'):>7}  {re.sub(r'[ ]+', ' ', stmt)[:100]}")
        return 0

    if args.refill_inlined:
        print(refill_inlined(args.duckdb, build, dsn, env, args.refill_inlined, args.refill_schemas))
        print()

    # per arm: shape -> [count, total ms]; plus the wall clock of the whole logged run
    totals = {arm: collections.defaultdict(lambda: [0, 0]) for arm in ARMS}
    walls = {arm: [] for arm in ARMS}
    # One attach warms the server's plan cache and the extension's catalog metadata, and the first
    # round costs 2-3x the rest - so the arm order flips every round and a warm-up round runs first,
    # or the warm-up lands entirely on whichever arm goes first and reads as that arm's cost.
    run_arm(args.duckdb, build, dsn, ARMS[0], env, args.read_only)
    for round_no in range(args.rounds):
        for arm in (ARMS if round_no % 2 == 0 else tuple(reversed(ARMS))):
            timed = run_arm(args.duckdb, build, dsn, arm, env, args.read_only)
            walls[arm].append(sum(ms for ms, _ in timed))
            for ms, statement in timed:
                entry = totals[arm][shape(redact(statement, dsn))]
                entry[0] += 1
                entry[1] += ms

    if args.union_sweep:
        print("one plan of N remote scans under UNION ALL, ms (LIMIT 0 - planning only)")
        print(f"{'branches':>9} {'ON':>7} {'OFF':>7} {'ON/OFF':>7}")
        for n in [int(x) for x in args.union_sweep.split(",")]:
            stmt = f"SELECT count(*) FROM ({union_sql(n, args.union_columns)}) u;"
            got = {}
            for arm in ARMS:
                timed = run_arm(args.duckdb, build, dsn, arm, env, args.read_only, stmt)
                got[arm] = max((ms for ms, m in timed if "UNION ALL" in m), default=0)
            ratio = got["true"] / got["false"] if got["false"] else 0
            print(f"{n:>9} {got['true']:>7} {got['false']:>7} {ratio:>7.2f}")
        return 0

    print(f"{'arm':<6} {'rounds':>7} {'wall ms (per round)':>24}")
    for arm in ARMS:
        print(f"{arm:<6} {len(walls[arm]):>7} {str(walls[arm]):>24}")
    print()
    ranked = sorted(totals["true"].items(), key=lambda kv: -kv[1][1])
    print(f"{'ON n':>5} {'ON ms':>7} {'OFF n':>6} {'OFF ms':>7}  shape")
    for shp, (count, ms) in ranked[: args.top]:
        off = totals["false"].get(shp, [0, 0])
        print(f"{count:>5} {ms:>7} {off[0]:>6} {off[1]:>7}  {shp[:110]}")
    # a shape that only the OFF arm ran would otherwise be invisible
    for shp, (count, ms) in sorted(totals["false"].items(), key=lambda kv: -kv[1][1]):
        if shp not in totals["true"]:
            print(f"{'-':>5} {'-':>7} {count:>6} {ms:>7}  {shp[:110]}")
    if args.full:
        print("\n--- full text of every shape, ON arm order ---")
        for shp, (count, ms) in ranked:
            off = totals["false"].get(shp, [0, 0])
            print(f"\n# ON: {count} x, {ms} ms   OFF: {off[0]} x, {off[1]} ms\n{shp}")

    if args.debug_counts:
        env2 = dict(env, MSSQL_DEBUG="2")
        with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as handle:
            out_csv = handle.name
        proc = subprocess.run([args.duckdb, "-unsigned", "-batch", "-no-agent"],
                              input=probe_sql(build, dsn, "true", out_csv), env=env2,
                              capture_output=True, text=True)
        log = proc.stdout + proc.stderr
        print("\n--- MSSQL_DEBUG=2, ON arm ---")
        for needle in ("SupportsPushdown(", "RemoteExecute", "PushdownRewriter", "remote_pushdown"):
            print(f"{log.count(needle):>8}  {needle}")
        print(f"{len(log.splitlines()):>8}  lines total")
        os.unlink(out_csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
