#!/usr/bin/env python3
"""Every metadata query DuckLake issues over a workload, by shape, with its cost and its path.

DuckLake logs each metadata query with its timing from `ExecuteRaw` - log type `DuckLakeMetadata`,
level DEBUG, a struct of catalog, query and elapsed_ms - and this extension's init path registers
the type. So nothing needs instrumenting: enable the log, run the workload, read `duckdb_logs()`.
This script does that around a SQL file and aggregates what came back.

    python3 scripts/bench/metadata_log.py workload.sql
    python3 scripts/bench/metadata_log.py workload.sql --top 20
    make metadata-log WORKLOAD=workload.sql

The workload is plain SQL run in one CLI session with the two extensions loaded; it should start
with its own ATTACH. Queries are grouped by shape - literals become '?', numbers become N, and
per-table inlined names collapse to ducklake_inlined_data_* - and each shape is classified by the
path it took:

    catalog     a read through DuckDB's catalog against the attached mssql database. Each table it
                touches is a separate scan, and each scan of a table the extension has not seen
                costs a metadata load first (design 002 section 1.1)
    mssql_scan  a read handed to the server as one T-SQL statement (specs/005 D13, 007, 008)
    execute     a write, DDL, or the attach itself

This is the instrument specs/008's numbers were taken with, and the one to reach for before
claiming where a workload's time goes. Two things it taught: `logging_storage` must be 'memory'
in the CLI (the default writes to stdout and `duckdb_logs()` stays empty), and a DATA_PATH with a
doubled slash is rejected by DuckLake, after which everything in the session fails quietly.
"""

import argparse
import collections
import re
import subprocess
import sys

LOAD = (
    "LOAD 'build/release/extension/mssql/mssql.duckdb_extension';\n"
    "LOAD 'build/release/extension/mssql_ducklake/mssql_ducklake.duckdb_extension';\n"
    "SET logging_storage = 'memory';\n"
    "SET enable_logging = true;\n"
    "SET logging_level = 'debug';\n"
    "SET enabled_log_types = 'DuckLakeMetadata';\n"
)
DUMP = "\nSELECT '\\x1e' || message FROM duckdb_logs() WHERE type = 'DuckLakeMetadata';\n"
MESSAGE = re.compile(r"'query': (?P<q>.*), 'elapsed_ms': (?P<ms>\d+)\}$", re.S)


def shape(query: str) -> str:
    s = re.sub(r"\s+", " ", query).strip()
    s = re.sub(r"'[^']*'", "'?'", s)
    s = re.sub(r"\b\d+\b", "N", s)
    s = re.sub(r"ducklake_inlined_(data|delete)_\w+", r"ducklake_inlined_\1_*", s)
    return s


def path_of(query: str) -> str:
    head = query.lstrip().split(None, 1)[0].upper() if query.strip() else ""
    if "mssql_scan(" in query:
        return "mssql_scan"
    if head in ("SELECT", "WITH", "FROM"):
        return "catalog"
    return "execute"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("workload", help="SQL file; runs in one session after the extensions load")
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--top", type=int, default=30, help="shapes to print, most expensive first")
    parser.add_argument("--width", type=int, default=150, help="characters of each shape to show")
    args = parser.parse_args()

    script = LOAD + open(args.workload).read() + DUMP
    proc = subprocess.run([args.duckdb, "-unsigned", "-csv", "-noheader"], input=script,
                          capture_output=True, text=True, timeout=7200)
    if proc.returncode != 0:
        sys.exit(f"the session failed:\n{proc.stderr[-2000:]}")
    # the dump's rows carry a record separator so the log's own commas and newlines cannot split them
    parsed = []
    for chunk in proc.stdout.split("\x1e")[1:]:
        text = chunk.strip().strip('"').replace('""', '"')
        m = MESSAGE.search(text)
        if not m:
            continue
        query = m.group("q").strip()
        if query.startswith("'") and query.endswith("'"):
            query = query[1:-1]
        parsed.append((query, int(m.group("ms"))))
    if not parsed:
        sys.exit("no DuckLakeMetadata entries came back - did the workload ATTACH a ducklake catalog?")

    agg = collections.OrderedDict()
    for query, ms in parsed:
        key = shape(query)
        a = agg.setdefault(key, {"n": 0, "ms": 0, "path": path_of(query)})
        a["n"] += 1
        a["ms"] += ms
    total = sum(ms for _, ms in parsed)
    by_path = collections.Counter()
    for a in agg.values():
        by_path[a["path"]] += a["ms"]

    print(f"{len(parsed)} metadata queries, {total} ms, {len(agg)} distinct shapes")
    print("  " + "   ".join(f"{p}: {by_path[p]} ms" for p in ("catalog", "mssql_scan", "execute")))
    print()
    print(f"{'n':>4} {'ms':>6}  {'path':<10} shape")
    for key, a in sorted(agg.items(), key=lambda kv: -kv[1]["ms"])[: args.top]:
        print(f"{a['n']:>4} {a['ms']:>6}  {a['path']:<10} {key[: args.width]}")


if __name__ == "__main__":
    main()
