#!/usr/bin/env bash
# Delete everything the Azure bench created (specs/021): the one resource group, and with it every
# VM, disk, server, registry, storage account and secret. Waits until Azure confirms the group is
# gone, then removes the run's local state (the generated passwords). The downloaded results stay.
#
#   scripts/bench/azure/down.sh               # BENCH_RG=<name> for another day's group
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/config.sh"

if ! az group show -n "$BENCH_RG" -o none 2>/dev/null; then
	echo "$BENCH_RG does not exist" >&2
else
	step "deleting $BENCH_RG"
	az resource list -g "$BENCH_RG" --query '[].{type:type, name:name}' -o table >&2
	az group delete -n "$BENCH_RG" --yes
	az group show -n "$BENCH_RG" -o none 2>/dev/null && die "$BENCH_RG still exists - check the portal"
	echo "$BENCH_RG deleted" >&2
fi
# a deleted Azure SQL logical server leaves nothing billable; a soft-deleted Key Vault would, but
# the bench creates none
rm -rf "$BENCH_STATE"
# anything else of the bench's still running in the subscription?
left="$(az group list --query "[?starts_with(name, 'mssql-ducklake-bench-')].name" -o tsv)"
[[ -z "$left" ]] || echo "other bench groups still exist: $left" >&2
