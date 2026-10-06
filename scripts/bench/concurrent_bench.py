#!/usr/bin/env python3
"""Many writers and readers through one DuckDB, against each catalog backend (specs/016).

`scale_catalog.py` measures one user: every phase is serial, one transaction at a time on one
connection, so each server works on one core. This measures the other shape: N threads of ONE
DuckDB process, each with its own connection, writing and reading the same lake at once.

    make bench-concurrent                                     # both backends, the default ladder
    make bench-concurrent CONC_ARGS='--backends mssql --threads 1,4,16'
    make bench-concurrent CONC_ARGS='--build ../mssql-ducklake-156/build/release'   # another line

The harness (concurrent_harness.c) is compiled against the build's own libduckdb, so the same
source measures any DuckDB line. Each run starts from an empty lake in a metadata schema of its own
(`bench_conc`), never `dbo` or the scale bench's `bench`.

Scenarios:
  own_inlined   N writers, each into its own table, 2 rows a commit (inlined)
  own_file      N writers, each into its own table, 200 rows a commit (a data file)
  shared_file   N writers into ONE table, 200 rows a commit: commits contend on the same table
  read          N readers, filtered reads over 16 pre-filled tables
  mixed         N/2 own_file writers and N/2 readers at once

Every scenario checks its result: the rows the writers committed must all be there. A run with an
error or a short count is reported as such - a benchmark that cannot fail proves nothing.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA = "bench_conc"
COLUMNS = 40
KINDS = ("BIGINT", "VARCHAR", "DECIMAL(18, 4)", "DATE", "DOUBLE")
EXPRS = ("{s}", "'v' || ({s})::VARCHAR", "({s} * 1.5)::DECIMAL(18, 4)", "DATE '2020-01-01' + ({s})::INTEGER",
         "({s} * 0.25)::DOUBLE")
COLS = ", ".join(f"c{c} {KINDS[c % 5]}" for c in range(COLUMNS))
VALS = ", ".join(EXPRS[c % 5].format(s="r") for c in range(COLUMNS))
READ_TABLES = 16


def harness_for(build: str, scratch: str) -> str:
    """The harness, compiled against this build's libduckdb (cached per build)."""
    build = os.path.abspath(build)
    root = os.path.dirname(os.path.dirname(build))
    out = os.path.join(scratch, "harness_" + re.sub(r"[^A-Za-z0-9]", "_", root)[-40:])
    if not os.path.exists(out):
        subprocess.run(["cc", "-O2", "-o", out, os.path.join(HERE, "concurrent_harness.c"),
                        f"-I{root}/duckdb/src/include", f"-L{build}/src", "-lduckdb",
                        f"-Wl,-rpath,{build}/src", "-lpthread"], check=True)
    return out


def setup_sql(backend: str, build: str, data_path: str, scenario: str, threads: int) -> str:
    load = ("LOAD '{b}/extension/mssql/mssql.duckdb_extension';\n"
            "LOAD '{b}/extension/mssql_ducklake/mssql_ducklake.duckdb_extension';\n").format(b=build)
    pg_build = f"{build}/extension/postgres_scanner/postgres_scanner.duckdb_extension"
    if backend == "postgres":
        load += f"LOAD '{pg_build}';\n" if os.path.exists(pg_build) else "INSTALL postgres; LOAD postgres;\n"
        reset = ("ATTACH '{PG_DSN}' AS pg (TYPE postgres);\n"
                 f"CALL postgres_execute('pg', 'DROP SCHEMA IF EXISTS {SCHEMA} CASCADE');\n"
                 f"CALL postgres_execute('pg', 'CREATE SCHEMA {SCHEMA}');\n"
                 "DETACH pg;\n")
        attach = f"ATTACH 'ducklake:postgres:{{PG_DSN}}' AS lake (DATA_PATH '{data_path}', METADATA_SCHEMA '{SCHEMA}');\n"
    else:
        reset = ("ATTACH '{MSSQL_DSN}' AS srv (TYPE mssql);\n"
                 "SELECT mssql_exec('srv', 'DECLARE @s NVARCHAR(MAX) = N''''; "
                 "SELECT @s += N''DROP TABLE '' + QUOTENAME(s.name) + N''.'' + QUOTENAME(t.name) + N'';'' "
                 "FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id "
                 f"WHERE s.name = ''{SCHEMA}''; EXEC sp_executesql @s; "
                 f"IF SCHEMA_ID(''{SCHEMA}'') IS NULL EXEC(''CREATE SCHEMA {SCHEMA}'');');\n"
                 "DETACH srv;\n")
        attach = f"ATTACH 'ducklake:mssql:{{MSSQL_DSN}}' AS lake (DATA_PATH '{data_path}', METADATA_SCHEMA '{SCHEMA}');\n"
    sql = load + reset + attach
    writers = writer_count(scenario, threads)
    for t in range(max(writers, 1)):
        sql += f"CREATE TABLE lake.w{t}(id BIGINT, {COLS});\n"
    if scenario == "shared_file":
        sql += f"CREATE TABLE lake.shared(id BIGINT, {COLS});\n"
    if reader_count(scenario, threads):
        for t in range(READ_TABLES):
            sql += f"CREATE TABLE lake.r{t}(id BIGINT, {COLS});\n"
            for f in range(5):
                sql += f"INSERT INTO lake.r{t} SELECT r, {VALS} FROM range({f * 200}, {f * 200 + 200}) t(r);\n"
    return sql


def writer_count(scenario: str, threads: int) -> int:
    if scenario in ("own_inlined", "own_file", "shared_file"):
        return threads
    if scenario == "mixed":
        return max(threads // 2, 1)
    return 0


def reader_count(scenario: str, threads: int) -> int:
    if scenario == "read":
        return threads
    if scenario == "mixed":
        return max(threads - threads // 2, 1)
    return 0


def writer_sql(scenario: str) -> tuple:
    """The writer's template and the rows one op commits."""
    if scenario == "own_inlined":
        return f"INSERT INTO lake.w{{t}} SELECT r, {VALS} FROM range({{i}} * 2, {{i}} * 2 + 2) t(r);", 2
    if scenario == "shared_file":
        return f"INSERT INTO lake.shared SELECT r, {VALS} FROM range({{i}} * 200, {{i}} * 200 + 200) t(r);", 200
    return f"INSERT INTO lake.w{{t}} SELECT r, {VALS} FROM range({{i}} * 200, {{i}} * 200 + 200) t(r);", 200


#! one filtered read of the reader's own table ({t} < READ_TABLES at every rung of the ladder), its
#! range moving with the iteration so no two reads are the same statement
READER_SQL = "SELECT count(*), sum(c0) FROM lake.r{t} WHERE id BETWEEN {i} * 10 AND {i} * 10 + 150;"


def check_sql(scenario: str, threads: int) -> str:
    if scenario == "shared_file":
        return "SELECT count(*) FROM lake.shared;"
    writers = writer_count(scenario, threads)
    if writers == 0:
        return "SELECT count(*) FROM lake.r0;"
    return "SELECT " + " + ".join(f"(SELECT count(*) FROM lake.w{t})" for t in range(writers)) + ";"


def redact(text: str) -> str:
    for key in ("MSSQL_DUCKLAKE_TEST_DSN", "MSSQL_DUCKLAKE_PG_DSN"):
        value = os.environ.get(key)
        if value:
            text = text.replace(value, f"<{key}>")
    text = re.sub(r"(?i)(password\s*=\s*)[^;'\s]*", r"\1***", text)
    return text


def run_one(harness: str, scratch: str, backend: str, build: str, scenario: str, threads: int, ops: int) -> dict:
    data_path = tempfile.mkdtemp(prefix=f"conc_{backend}_{scenario}_{threads}_", dir=scratch)
    files = {}
    for name, text in (("setup", setup_sql(backend, build, data_path, scenario, threads)),
                       ("writer", writer_sql(scenario)[0]), ("reader", READER_SQL),
                       ("check", check_sql(scenario, threads))):
        path = os.path.join(data_path, f"{name}.sql")
        with open(path, "w") as f:
            f.write(text)
        files[name] = path
    writers, readers = writer_count(scenario, threads), reader_count(scenario, threads)
    proc = subprocess.run([harness, files["setup"], files["writer"] if writers else "-", str(writers),
                           files["reader"] if readers else "-", str(readers), str(ops), files["check"]],
                          capture_output=True, text=True, timeout=3600)
    out = redact(proc.stdout + proc.stderr)
    result = {"roles": {}, "errors": [], "check": None, "raw": out}
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] in ("writer", "reader") and len(parts) == 9:
            result["roles"][parts[0]] = dict(zip(("threads", "ops", "errors", "wall", "ops_s", "p50", "p95", "max"),
                                                 [float(x) for x in parts[1:]]))
        elif line.startswith("first_error"):
            result["errors"].append(line)
        elif line.startswith("check "):
            result["check"] = int(parts[1]) if parts[1].isdigit() else parts[1]
        elif line.startswith(("check_error", "setup failed", "open failed")):
            result["errors"].append(line)
    if proc.returncode != 0 and not result["errors"]:
        result["errors"].append(f"exit {proc.returncode}: {out[-400:]}")
    # the rows the writers must have committed
    if writers:
        expected = writers * ops * writer_sql(scenario)[1]
        if result["check"] != expected:
            result["errors"].append(f"rows: expected {expected}, found {result['check']}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default="build/release")
    parser.add_argument("--backends", default="mssql,postgres")
    parser.add_argument("--scenarios", default="own_inlined,own_file,shared_file,read,mixed")
    parser.add_argument("--threads", default="1,2,4,8,16")
    parser.add_argument("--ops", type=int, default=20, help="operations per thread")
    parser.add_argument("--label", default="", help="a name for this build in the output")
    args = parser.parse_args()
    build = os.path.abspath(args.build)
    scratch = tempfile.mkdtemp(prefix="conc_")
    harness = harness_for(build, scratch)
    label = args.label or build
    print(f"# {label}: {args.ops} ops a thread")
    print(f"{'backend':8} {'scenario':12} {'N':>3}  {'role':6} {'ops/s':>8} {'p50 ms':>8} {'p95 ms':>8} {'max ms':>8} "
          f"{'wall s':>7}  result")
    failed = False
    for backend in [b.strip() for b in args.backends.split(",") if b.strip()]:
        for scenario in [s.strip() for s in args.scenarios.split(",") if s.strip()]:
            for threads in [int(n) for n in args.threads.split(",")]:
                r = run_one(harness, scratch, backend, build, scenario, threads, args.ops)
                status = "ok" if not r["errors"] else "FAIL " + "; ".join(r["errors"])[:200]
                failed |= bool(r["errors"])
                if not r["roles"]:
                    print(f"{backend:8} {scenario:12} {threads:>3}  {'-':6} {'':>8} {'':>8} {'':>8} {'':>8} {'':>7}  {status}")
                for role, m in r["roles"].items():
                    print(f"{backend:8} {scenario:12} {threads:>3}  {role:6} {m['ops_s']:>8.1f} {m['p50']:>8.1f} "
                          f"{m['p95']:>8.1f} {m['max']:>8.1f} {m['wall']:>7.2f}  {status}")
                sys.stdout.flush()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
