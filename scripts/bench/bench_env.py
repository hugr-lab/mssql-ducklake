"""What a bench run takes from its environment beyond the DSNs: where the lake's files go, and SQL to
run before anything else. Both are empty on a laptop, where the benches keep their parquet in a local
temporary directory as they always have. The Azure run (specs/021) sets them: the files go to blob
storage, and the preamble loads the azure extension and creates the secret that reaches it.

    MSSQL_DUCKLAKE_BENCH_DATA_ROOT  a root for every lake the benches create, e.g.
                                    abfss://lake-mssql@<account>.dfs.core.windows.net/run-<id>
    MSSQL_DUCKLAKE_BENCH_PREAMBLE   SQL run right after the extensions load, e.g.
                                    LOAD '.../azure.duckdb_extension'; CREATE SECRET (...);
"""

import os
import tempfile
import uuid


def preamble() -> str:
    text = os.environ.get("MSSQL_DUCKLAKE_BENCH_PREAMBLE", "").strip()
    return text + "\n" if text else ""


def data_path(name: str, scratch: str = None) -> str:
    """A fresh place for one lake's files: under the data root when there is one, else a local
    temporary directory (in `scratch` when given)."""
    root = os.environ.get("MSSQL_DUCKLAKE_BENCH_DATA_ROOT", "").strip()
    if root:
        return f"{root.rstrip('/')}/{name}_{uuid.uuid4().hex[:8]}"
    return tempfile.mkdtemp(prefix=f"{name}_", dir=scratch)


def pg_load(build: str = "build/release") -> str:
    """The postgres extension: the build's own when there is one (a dev duckdb has no published
    postgres_scanner to INSTALL - specs/015 builds it from source), otherwise the released one."""
    built = f"{build}/extension/postgres_scanner/postgres_scanner.duckdb_extension"
    return f"LOAD '{built}';\n" if os.path.exists(built) else "INSTALL postgres; LOAD postgres;\n"
