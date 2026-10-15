#!/usr/bin/env bash
# Run one workload as an execution of the bench job, wait for it, and download its result
# (specs/021). The workload and its arguments are the entry point's (entrypoint.sh):
#
#   scripts/bench/azure/run.sh info
#   scripts/bench/azure/run.sh scale --tables 1000
#   scripts/bench/azure/run.sh compare --repeat 3
#   scripts/bench/azure/run.sh concurrent --threads 1,2,4,8,16
#   BENCH_TARGET=managed scripts/bench/azure/run.sh scale --tables 1000
#
# One replica per execution. Several replicas against one catalog (separate nodes) need a workload
# that does not reset the catalog in every replica - a follow-up of specs/021.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/config.sh"
bench_names
[[ $# -ge 1 ]] || die "usage: run.sh <info|scale|compare|concurrent> [args...]"

step "starting $* (target: ${BENCH_TARGET:-vm})"
# The execution is started through the REST API with its container spelled out: `az containerapp
# job start --args` reads any value that starts with a dash as its own option (`--tables` is not
# one), and the spec it would send replaces the job's container whole anyway. So the job's own
# container is read and sent back with the workload's command and arguments - its image, resources
# and environment as the job holds them, the secrets as references, whose values are never read.
job_id="$(az containerapp job show -g "$BENCH_RG" -n "$BENCH_JOB" --query id -o tsv)"
body="$(az containerapp job show -g "$BENCH_RG" -n "$BENCH_JOB" --query 'properties.template.containers[0]' -o json |
	python3 -c '
import json, sys
container = json.load(sys.stdin)
target = sys.argv[1]
env = [e for e in container.get("env", []) if e["name"] != "BENCH_TARGET"]
env.append({"name": "BENCH_TARGET", "value": target})
print(json.dumps({"containers": [{
    "name": container["name"],
    "image": container["image"],
    "command": ["/opt/mssql-ducklake/scripts/bench/azure/entrypoint.sh"],
    "args": sys.argv[2:],
    "env": env,
    "resources": container["resources"],
    **({"volumeMounts": container["volumeMounts"]} if container.get("volumeMounts") else {}),
}]}))
' "${BENCH_TARGET:-vm}" "$@")"
execution="$(az rest --method post --url "https://management.azure.com${job_id}/start?api-version=2024-03-01" \
	--body "$body" --query name -o tsv)"
echo "execution $execution" >&2

while true; do
	status="$(az containerapp job execution show -g "$BENCH_RG" -n "$BENCH_JOB" --job-execution-name "$execution" \
		--query properties.status -o tsv)"
	case "$status" in
	Succeeded | Failed | Stopped | Degraded) break ;;
	esac
	sleep 30
done
echo "execution $execution: $status" >&2

results="$BENCH_DIR/results/$BENCH_RG/$execution"
mkdir -p "$results"
az storage blob download-batch --account-name "$BENCH_STORAGE" --auth-mode login -s results \
	-d "$BENCH_DIR/results/$BENCH_RG" --pattern "${execution}/*" -o none
cat "$results"/*.txt 2>/dev/null || echo "no result uploaded: az containerapp job logs show -g $BENCH_RG -n $BENCH_JOB --execution $execution" >&2
[[ "$status" == Succeeded ]]
