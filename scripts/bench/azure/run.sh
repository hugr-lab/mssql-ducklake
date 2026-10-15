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
# An execution that overrides the command replaces the job's container spec whole, so the image,
# resources and environment are passed again as the job holds them - the secrets as references
# (secretref:<name>), whose values this script never reads.
container="$(az containerapp job show -g "$BENCH_RG" -n "$BENCH_JOB" --query 'properties.template.containers[0]' -o json)"
spec() { python3 -c "import json,sys; c=json.load(sys.stdin); print($1)" <<<"$container"; }
image="$(spec "c['image']")"
cpu="$(spec "c['resources']['cpu']")"
memory="$(spec "c['resources']['memory']")"
env_vars=()
while IFS= read -r line; do
	[[ -n "$line" && "$line" != BENCH_TARGET=* ]] && env_vars+=("$line")
done < <(spec "'\\n'.join(e['name'] + '=' + ('secretref:' + e['secretRef'] if e.get('secretRef') else e.get('value', '')) for e in c.get('env', []))")
env_vars+=("BENCH_TARGET=${BENCH_TARGET:-vm}")
execution="$(az containerapp job start -g "$BENCH_RG" -n "$BENCH_JOB" --image "$image" --cpu "$cpu" --memory "$memory" \
	--command /opt/mssql-ducklake/scripts/bench/azure/entrypoint.sh --args "$@" \
	--env-vars "${env_vars[@]}" --query name -o tsv)"
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
