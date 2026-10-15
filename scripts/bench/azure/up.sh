#!/usr/bin/env bash
# Create everything the Azure bench needs (specs/021), in one resource group:
#
#   budget alert -> network (VNet, NSG, NAT) -> storage -> identity
#   -> the two catalog VMs -> the Container Apps environment and the bench job
#   [-> the managed arm: Azure SQL Database + PostgreSQL Flexible Server, BENCH_MANAGED=1]
#
# Each step is safe to run again. The passwords are generated into .state/<rg>/secrets.env (mode
# 600) and reach the job as Container Apps secrets; nothing here prints them. Tear down with
# down.sh - everything lives in the one resource group.
#
#   git push origin bench/<name>                       # the image, built on GitHub (once per commit)
#   scripts/bench/azure/up.sh                          # BENCH_REF=<that commit>
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/config.sh"
command -v az >/dev/null || die "the Azure CLI (az) is needed"
az account show >/dev/null 2>&1 || die "not signed in: az login"
bench_names
mkdir -p "$BENCH_STATE"
chmod 700 "$BENCH_STATE"

# --- secrets, generated once per resource group -------------------------------------------------
SECRETS="$BENCH_STATE/secrets.env"
if [[ ! -f "$SECRETS" ]]; then
	umask 077
	gen() { openssl rand -base64 48 | tr -dc 'A-Za-z0-9' | head -c 28; }
	{
		# SQL Server's policy wants three character classes; the suffix guarantees them
		echo "BENCH_MSSQL_PASS='$(gen)Aa1'"
		echo "BENCH_PG_PASS='$(gen)'"
		echo "BENCH_MANAGED_PASS='$(gen)Aa1'"
	} >"$SECRETS"
fi
# shellcheck disable=SC1090
source "$SECRETS"

TAGS=(purpose=bench owner="$(az account show --query user.name -o tsv)" spec=021)

step "resource group $BENCH_RG in $BENCH_LOCATION"
az group create -n "$BENCH_RG" -l "$BENCH_LOCATION" --tags "${TAGS[@]}" -o none
RG_ID="$(az group show -n "$BENCH_RG" --query id -o tsv)"

step "budget alert at \$$BENCH_BUDGET_USD"
ME="$(az ad signed-in-user show --query mail -o tsv 2>/dev/null || true)"
[[ -n "$ME" && "$ME" != "None" ]] || ME="$(az account show --query user.name -o tsv)"
START="$(date -u +%Y-%m-01)"
END="$(date -u -v+1y +%Y-%m-01 2>/dev/null || date -u -d '+1 year' +%Y-%m-01)"
az rest --method put -o none \
	--url "https://management.azure.com${RG_ID}/providers/Microsoft.Consumption/budgets/mssql-ducklake-bench?api-version=2023-05-01" \
	--body "{\"properties\":{\"category\":\"Cost\",\"amount\":${BENCH_BUDGET_USD},\"timeGrain\":\"Monthly\",
		\"timePeriod\":{\"startDate\":\"${START}T00:00:00Z\",\"endDate\":\"${END}T00:00:00Z\"},
		\"notifications\":{\"actual50\":{\"enabled\":true,\"operator\":\"GreaterThan\",\"threshold\":50,\"contactEmails\":[\"${ME}\"]},
		\"actual100\":{\"enabled\":true,\"operator\":\"GreaterThan\",\"threshold\":100,\"contactEmails\":[\"${ME}\"]}}}}"

step "network: VNet, the servers' NSG, a NAT gateway for outbound"
az network vnet create -g "$BENCH_RG" -n "$BENCH_VNET" --address-prefixes 10.42.0.0/16 \
	--subnet-name servers --subnet-prefixes 10.42.1.0/24 --tags "${TAGS[@]}" -o none
az network vnet subnet create -g "$BENCH_RG" --vnet-name "$BENCH_VNET" -n aca \
	--address-prefixes 10.42.4.0/23 --delegations Microsoft.App/environments -o none 2>/dev/null ||
	az network vnet subnet update -g "$BENCH_RG" --vnet-name "$BENCH_VNET" -n aca \
		--delegations Microsoft.App/environments -o none
az network public-ip create -g "$BENCH_RG" -n mssql-ducklake-nat-ip --sku Standard -o none
az network nat gateway create -g "$BENCH_RG" -n mssql-ducklake-nat --public-ip-addresses mssql-ducklake-nat-ip -o none
for subnet in servers aca; do
	az network vnet subnet update -g "$BENCH_RG" --vnet-name "$BENCH_VNET" -n "$subnet" \
		--nat-gateway mssql-ducklake-nat -o none
done
# the catalog servers answer the job's subnet and nothing else: no public IP, no port from outside
az network nsg create -g "$BENCH_RG" -n mssql-ducklake-servers-nsg -o none
az network nsg rule create -g "$BENCH_RG" --nsg-name mssql-ducklake-servers-nsg -n catalog-from-aca \
	--priority 100 --direction Inbound --access Allow --protocol Tcp \
	--source-address-prefixes 10.42.4.0/23 --destination-port-ranges 1433 5432 -o none
az network nsg rule create -g "$BENCH_RG" --nsg-name mssql-ducklake-servers-nsg -n deny-rest-of-vnet \
	--priority 4000 --direction Inbound --access Deny --protocol '*' \
	--source-address-prefixes VirtualNetwork --destination-port-ranges '*' -o none
az network vnet subnet update -g "$BENCH_RG" --vnet-name "$BENCH_VNET" -n servers \
	--network-security-group mssql-ducklake-servers-nsg -o none

step "storage account $BENCH_STORAGE (ADLS Gen2): lakes, results"
az storage account create -g "$BENCH_RG" -n "$BENCH_STORAGE" -l "$BENCH_LOCATION" --sku Standard_LRS \
	--kind StorageV2 --hns true --min-tls-version TLS1_2 --allow-blob-public-access false \
	--tags "${TAGS[@]}" -o none
STORAGE_ID="$(az storage account show -g "$BENCH_RG" -n "$BENCH_STORAGE" --query id -o tsv)"
# the operator reads the results with their own login
ME_ID="$(az ad signed-in-user show --query id -o tsv)"
az role assignment create --assignee-object-id "$ME_ID" --assignee-principal-type User \
	--role "Storage Blob Data Contributor" --scope "$STORAGE_ID" -o none 2>/dev/null || true
for container in lakes results; do
	# the role takes a minute to reach the data plane
	for _ in $(seq 1 20); do
		az storage container create --account-name "$BENCH_STORAGE" -n "$container" --auth-mode login -o none 2>/dev/null && break
		sleep 15
	done
done
if [[ "$BENCH_DATA" == files ]]; then
	az storage share-rm create -g "$BENCH_RG" --storage-account "$BENCH_STORAGE" -n lakes --quota 100 -o none
fi

step "identity $BENCH_IDENTITY: blob data"
az identity create -g "$BENCH_RG" -n "$BENCH_IDENTITY" -o none
IDENTITY_ID="$(az identity show -g "$BENCH_RG" -n "$BENCH_IDENTITY" --query id -o tsv)"
IDENTITY_PRINCIPAL="$(az identity show -g "$BENCH_RG" -n "$BENCH_IDENTITY" --query principalId -o tsv)"
IDENTITY_CLIENT="$(az identity show -g "$BENCH_RG" -n "$BENCH_IDENTITY" --query clientId -o tsv)"
az role assignment create --assignee-object-id "$IDENTITY_PRINCIPAL" --assignee-principal-type ServicePrincipal \
	--role "Storage Blob Data Contributor" --scope "$STORAGE_ID" -o none 2>/dev/null || true

step "the bench image for $BENCH_REF (built by .github/workflows/bench-image.yml, pulled from GHCR)"
REF_SHA="$(git ls-remote https://github.com/hugr-lab/mssql-ducklake "$BENCH_REF" | cut -f1 | head -1)"
[[ -n "$REF_SHA" ]] || REF_SHA="$BENCH_REF"
IMAGE="${BENCH_IMAGE:-ghcr.io/hugr-lab/mssql-ducklake-bench:${REF_SHA:0:12}}"
# the package is public: Container Apps pulls it without a credential
token="$(curl -fsS "https://ghcr.io/token?scope=repository:hugr-lab/mssql-ducklake-bench:pull" | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])' || true)"
if [[ -z "$token" ]] || ! curl -fsS -o /dev/null -H "Authorization: Bearer $token" \
	-H "Accept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.v2+json" \
	"https://ghcr.io/v2/hugr-lab/mssql-ducklake-bench/manifests/${IMAGE##*:}"; then
	die "no image $IMAGE to pull without a credential: push a bench/ branch at $REF_SHA (bench-image.yml)"
fi

step "the catalog VMs: $BENCH_VM_MSSQL and $BENCH_VM_PG ($BENCH_VM_SIZE each)"
render() { # the cloud-init template with this run's values, kept in the state directory
	sed -e "s|@MSSQL_IMAGE@|${BENCH_MSSQL_IMAGE}|g" -e "s|@PG_IMAGE@|${BENCH_PG_IMAGE}|g" \
		-e "s|@MEMORY_MB@|${BENCH_SERVER_MEMORY_MB}|g" -e "s|@MSSQL_PASS@|${BENCH_MSSQL_PASS}|g" \
		-e "s|@PG_PASS@|${BENCH_PG_PASS}|g" "$BENCH_DIR/$1" >"$BENCH_STATE/$1"
	chmod 600 "$BENCH_STATE/$1"
	echo "$BENCH_STATE/$1"
}
# a key of the run's own, not the operator's: the VMs have no public IP, the key is for the serial
# console at most, and it goes with the state directory
[[ -f "$BENCH_STATE/vm_key" ]] || ssh-keygen -q -t ed25519 -N "" -C mssql-ducklake-bench -f "$BENCH_STATE/vm_key"
for pair in "$BENCH_VM_MSSQL:cloud-init-sqlserver.yaml" "$BENCH_VM_PG:cloud-init-postgres.yaml"; do
	vm="${pair%%:*}"
	# the v6 sizes attach their disks through NVMe only
	controller=()
	[[ "$BENCH_VM_SIZE" == *_v6 ]] && controller=(--disk-controller-type NVMe)
	if ! az vm show -g "$BENCH_RG" -n "$vm" -o none 2>/dev/null; then
		az vm create -g "$BENCH_RG" -n "$vm" --image Ubuntu2404 --size "$BENCH_VM_SIZE" "${controller[@]}" \
			--vnet-name "$BENCH_VNET" --subnet servers --public-ip-address "" --nsg "" \
			--os-disk-size-gb 128 --storage-sku Premium_LRS --admin-username bench \
			--ssh-key-values "$BENCH_STATE/vm_key.pub" --custom-data "$(render "${pair#*:}")" --tags "${TAGS[@]}" -o none
	fi
done
MSSQL_IP="$(az vm list-ip-addresses -g "$BENCH_RG" -n "$BENCH_VM_MSSQL" --query '[0].virtualMachine.network.privateIpAddresses[0]' -o tsv)"
PG_IP="$(az vm list-ip-addresses -g "$BENCH_RG" -n "$BENCH_VM_PG" --query '[0].virtualMachine.network.privateIpAddresses[0]' -o tsv)"

step "waiting for both servers to answer (cloud-init pulls the images and creates the databases)"
for vm in "$BENCH_VM_MSSQL" "$BENCH_VM_PG"; do
	for _ in $(seq 1 60); do
		state="$(az vm run-command invoke -g "$BENCH_RG" -n "$vm" --command-id RunShellScript \
			--scripts 'test -f /var/lib/mssql-ducklake-ready && echo ready || echo waiting' \
			--query 'value[0].message' -o tsv 2>/dev/null || true)"
		[[ "$state" == *ready* && "$state" != *waiting* ]] && break
		sleep 20
	done
	[[ "$state" == *ready* && "$state" != *waiting* ]] || die "$vm did not come up: see az vm boot-diagnostics / cloud-init log"
	echo "$vm ready" >&2
done

step "Container Apps environment $BENCH_ENV, profile $BENCH_PROFILE_TYPE"
az monitor log-analytics workspace create -g "$BENCH_RG" -n "$BENCH_LOGS" --retention-time 30 -o none
LOGS_ID="$(az monitor log-analytics workspace show -g "$BENCH_RG" -n "$BENCH_LOGS" --query customerId -o tsv)"
LOGS_KEY="$(az monitor log-analytics workspace get-shared-keys -g "$BENCH_RG" -n "$BENCH_LOGS" --query primarySharedKey -o tsv)"
ACA_SUBNET="$(az network vnet subnet show -g "$BENCH_RG" --vnet-name "$BENCH_VNET" -n aca --query id -o tsv)"
if ! az containerapp env show -g "$BENCH_RG" -n "$BENCH_ENV" -o none 2>/dev/null; then
	az containerapp env create -g "$BENCH_RG" -n "$BENCH_ENV" -l "$BENCH_LOCATION" \
		--infrastructure-subnet-resource-id "$ACA_SUBNET" --internal-only true --enable-workload-profiles \
		--logs-workspace-id "$LOGS_ID" --logs-workspace-key "$LOGS_KEY" --tags "${TAGS[@]}" -o none
fi
az containerapp env workload-profile add -g "$BENCH_RG" -n "$BENCH_ENV" --workload-profile-name bench \
	--workload-profile-type "$BENCH_PROFILE_TYPE" --min-nodes 0 --max-nodes "$BENCH_PROFILE_MAX_NODES" -o none 2>/dev/null || true
if [[ "$BENCH_DATA" == files ]]; then
	KEY="$(az storage account keys list -g "$BENCH_RG" -n "$BENCH_STORAGE" --query '[0].value' -o tsv)"
	az containerapp env storage set -g "$BENCH_RG" -n "$BENCH_ENV" --storage-name lakes \
		--azure-file-account-name "$BENCH_STORAGE" --azure-file-account-key "$KEY" \
		--azure-file-share-name lakes --access-mode ReadWrite -o none
fi

MSSQL_DSN="Server=${MSSQL_IP},1433;Database=lake_meta;User Id=sa;Password=${BENCH_MSSQL_PASS};TrustServerCertificate=yes"
PG_DSN="dbname=lake_meta host=${PG_IP} port=5432 user=ducklake password=${BENCH_PG_PASS}"

if [[ "$BENCH_MANAGED" == 1 ]]; then
	step "the managed arm: Azure SQL Database GP 4 vCore, PostgreSQL Flexible GP D4ds_v5"
	az network vnet subnet update -g "$BENCH_RG" --vnet-name "$BENCH_VNET" -n aca --service-endpoints Microsoft.Sql -o none
	az sql server create -g "$BENCH_RG" -n "mssql-ducklake-${BENCH_SUFFIX}" -l "$BENCH_LOCATION" \
		--admin-user benchadmin --admin-password "$BENCH_MANAGED_PASS" -o none
	az sql server vnet-rule create -g "$BENCH_RG" -s "mssql-ducklake-${BENCH_SUFFIX}" -n from-aca \
		--vnet-name "$BENCH_VNET" --subnet aca -o none
	az sql db create -g "$BENCH_RG" -s "mssql-ducklake-${BENCH_SUFFIX}" -n lake_meta --edition GeneralPurpose \
		--family Gen5 --capacity 4 --compute-model Provisioned --zone-redundant false -o none
	az postgres flexible-server create -g "$BENCH_RG" -n "mssql-ducklake-pg-${BENCH_SUFFIX}" -l "$BENCH_LOCATION" \
		--tier GeneralPurpose --sku-name Standard_D4ds_v5 --version 17 --storage-size 128 \
		--admin-user ducklake --admin-password "$BENCH_MANAGED_PASS" \
		--vnet "$BENCH_VNET" --subnet pgflex --address-prefixes 10.42.8.0/24 --yes -o none
	az postgres flexible-server db create -g "$BENCH_RG" -s "mssql-ducklake-pg-${BENCH_SUFFIX}" -d lake_meta -o none
	MANAGED_MSSQL_DSN="Server=mssql-ducklake-${BENCH_SUFFIX}.database.windows.net,1433;Database=lake_meta;User Id=benchadmin;Password=${BENCH_MANAGED_PASS};Encrypt=yes"
	MANAGED_PG_DSN="dbname=lake_meta host=mssql-ducklake-pg-${BENCH_SUFFIX}.postgres.database.azure.com port=5432 user=ducklake password=${BENCH_MANAGED_PASS} sslmode=require"
fi

step "the bench job $BENCH_JOB"
SECRET_ARGS=(mssql-dsn="$MSSQL_DSN" pg-dsn="$PG_DSN")
[[ "$BENCH_MANAGED" == 1 ]] && SECRET_ARGS+=(managed-mssql-dsn="$MANAGED_MSSQL_DSN" managed-pg-dsn="$MANAGED_PG_DSN")
ENV_ARGS=(MSSQL_DUCKLAKE_TEST_DSN=secretref:mssql-dsn MSSQL_DUCKLAKE_PG_DSN=secretref:pg-dsn
	BENCH_STORAGE_ACCOUNT="$BENCH_STORAGE" BENCH_DATA="$BENCH_DATA" AZURE_CLIENT_ID="$IDENTITY_CLIENT"
	BENCH_VM_SIZE="$BENCH_VM_SIZE")
[[ "$BENCH_MANAGED" == 1 ]] && ENV_ARGS+=(BENCH_MANAGED_MSSQL_DSN=secretref:managed-mssql-dsn BENCH_MANAGED_PG_DSN=secretref:managed-pg-dsn)
if ! az containerapp job show -g "$BENCH_RG" -n "$BENCH_JOB" -o none 2>/dev/null; then
	az containerapp job create -g "$BENCH_RG" -n "$BENCH_JOB" --environment "$BENCH_ENV" \
		--workload-profile-name bench --trigger-type Manual --replica-timeout 14400 --replica-retry-limit 0 \
		--replica-completion-count 1 --parallelism 1 --image "$IMAGE" --cpu 3.5 --memory 12Gi \
		--mi-user-assigned "$IDENTITY_ID" --secrets "${SECRET_ARGS[@]}" --env-vars "${ENV_ARGS[@]}" \
		--tags "${TAGS[@]}" -o none
else
	az containerapp job secret set -g "$BENCH_RG" -n "$BENCH_JOB" --secrets "${SECRET_ARGS[@]}" -o none
	az containerapp job update -g "$BENCH_RG" -n "$BENCH_JOB" --image "$IMAGE" --set-env-vars "${ENV_ARGS[@]}" -o none
fi
if [[ "$BENCH_DATA" == files ]]; then
	# mount the share at /mnt/lakes: the job's YAML is the only place a volume can be declared
	az containerapp job show -g "$BENCH_RG" -n "$BENCH_JOB" -o yaml >"$BENCH_STATE/job.yaml"
	python3 - "$BENCH_STATE/job.yaml" <<'PY'
import sys, re
path = sys.argv[1]
text = open(path).read()
if "volumeMounts" not in text:
    text = re.sub(r"(\n(\s+)image: .*\n)", r"\1\2volumeMounts:\n\2- mountPath: /mnt/lakes\n\2  volumeName: lakes\n", text, count=1)
    text = re.sub(r"(\n(\s+)containers:\n)", r"\n\2volumes:\n\2- name: lakes\n\2  storageName: lakes\n\2  storageType: AzureFile\1", text, count=1)
open(path, "w").write(text)
PY
	az containerapp job update -g "$BENCH_RG" -n "$BENCH_JOB" --yaml "$BENCH_STATE/job.yaml" -o none
fi

cat >"$BENCH_STATE/outputs.env" <<EOF
BENCH_RG=$BENCH_RG
BENCH_STORAGE=$BENCH_STORAGE
BENCH_JOB=$BENCH_JOB
BENCH_IMAGE=$IMAGE
BENCH_REF_SHA=$REF_SHA
EOF
step "ready: scripts/bench/azure/run.sh info, then run.sh scale --tables 1000 (see README.md)"
