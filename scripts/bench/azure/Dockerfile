# The Azure bench image (specs/021), runtime only. Nothing is compiled here: .github/workflows/
# bench-image.yml builds duckdb with mssql_ducklake, mssql, postgres_scanner and azure on a GitHub
# runner (the same Ubuntu 24.04 as this base), stages the binaries under image/opt/mssql-ducklake in
# the repository's own layout, and builds this file with image/ as the context.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
# gcc: concurrent_bench.py compiles its harness against the build's libduckdb at run time;
# azcopy: the entry point uploads the result with the job's managed identity
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 ca-certificates curl gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/* \
    && curl -sSL https://aka.ms/downloadazcopy-v10-linux | tar -xz -C /tmp \
    && mv /tmp/azcopy_linux_amd64_*/azcopy /usr/local/bin/ && rm -rf /tmp/azcopy_linux_amd64_*
COPY opt/mssql-ducklake /opt/mssql-ducklake
WORKDIR /opt/mssql-ducklake
ENTRYPOINT ["/opt/mssql-ducklake/scripts/bench/azure/entrypoint.sh"]
CMD ["info"]
