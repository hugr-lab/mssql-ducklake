PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=mssql_ducklake
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Build dependencies come from vcpkg, the standard duckdb-extension dependency manager: the
# merged-manifest step collects the vcpkg.json of each loaded extension (roaring from ducklake,
# openssl/simdutf from mssql), so nothing is listed here by hand. Bootstrap once with
# `make vcpkg-setup`, or point VCPKG_TOOLCHAIN_PATH at an existing checkout.
USE_MERGED_VCPKG_MANIFEST := 1
VCPKG_TOOLCHAIN_PATH ?= $(PROJ_DIR)vcpkg/scripts/buildsystems/vcpkg.cmake

# Only the goals that configure cmake need the toolchain; checkout/CI helper goals run before any
# vcpkg exists (the duckdb-acl repo learned this the hard way - name our goals, not the exceptions).
GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
VCPKG_GOALS := all release debug reldebug relassert
ifeq ($(wildcard $(VCPKG_TOOLCHAIN_PATH)),)
ifneq ($(filter $(VCPKG_GOALS),$(GOALS)),)
$(error this build needs vcpkg: run 'make vcpkg-setup' first, or set VCPKG_TOOLCHAIN_PATH)
endif
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# --- integration environment (docker/docker-compose.yml) ----------------------------------------
# One SQL Server holding the DuckLake catalog. `.env` (from .env.example) overrides the defaults;
# the same variables feed the compose file and the connection string the tests gate on. Make keeps
# the quotes a dotenv habit adds (`X="y"` is the value `"y"` to make), so strip them here.
-include .env
unquote = $(patsubst "%",%,$(patsubst '%',%,$(1)))
MSSQL_DUCKLAKE_HOST := $(call unquote,$(if $(MSSQL_DUCKLAKE_HOST),$(MSSQL_DUCKLAKE_HOST),localhost))
MSSQL_DUCKLAKE_PORT := $(call unquote,$(if $(MSSQL_DUCKLAKE_PORT),$(MSSQL_DUCKLAKE_PORT),7433))
MSSQL_DUCKLAKE_PASS := $(call unquote,$(if $(MSSQL_DUCKLAKE_PASS),$(MSSQL_DUCKLAKE_PASS),TestPassword1))
MSSQL_DUCKLAKE_DB := $(call unquote,$(if $(MSSQL_DUCKLAKE_DB),$(MSSQL_DUCKLAKE_DB),lake_meta))
MSSQL_DUCKLAKE_IMAGE := $(call unquote,$(MSSQL_DUCKLAKE_IMAGE))

# The metadata connection string, ADO form: the tests put it behind the `ducklake:mssql:` prefix,
# and it is that `mssql:` (not `mssql://`, which duckdb deliberately leaves alone) that duckdb strips
# to pick the mssql storage for the catalog ATTACH. The login is sa - the only one the container has.
MSSQL_DUCKLAKE_TEST_DSN ?= Server=$(MSSQL_DUCKLAKE_HOST),$(MSSQL_DUCKLAKE_PORT);Database=$(MSSQL_DUCKLAKE_DB);User Id=sa;Password=$(MSSQL_DUCKLAKE_PASS)

DOCKER_COMPOSE := docker compose -f $(PROJ_DIR)docker/docker-compose.yml

.PHONY: docker-up docker-down docker-status test-integration
# only the docker goals see the variables (the password stays out of every build process's
# environment); the assignment form is the one make 3.81 (macOS) accepts for target-specific exports
docker-up docker-down docker-status: export MSSQL_DUCKLAKE_PORT := $(MSSQL_DUCKLAKE_PORT)
docker-up docker-down docker-status: export MSSQL_DUCKLAKE_PASS := $(MSSQL_DUCKLAKE_PASS)
docker-up docker-down docker-status: export MSSQL_DUCKLAKE_DB := $(MSSQL_DUCKLAKE_DB)
docker-up docker-down docker-status: export MSSQL_DUCKLAKE_IMAGE := $(MSSQL_DUCKLAKE_IMAGE)
# `run --rm` starts sqlserver, waits for its health check (depends_on), streams the init's output,
# propagates its exit code and removes the one-shot container
docker-up:
	$(DOCKER_COMPOSE) run --rm sqlserver-init

docker-down:
	$(DOCKER_COMPOSE) down

docker-status:
	$(DOCKER_COMPOSE) ps

# The server-backed suite (test/sql/integration/): gated on MSSQL_DUCKLAKE_TEST_DSN, so `make test`
# skips it and this target is the one that provides it. The floor fails a run that skipped anyway.
test-integration: export MSSQL_DUCKLAKE_TEST_DSN := $(MSSQL_DUCKLAKE_TEST_DSN)
test-integration:
	@test -x build/release/test/unittest || { echo "build first: GEN=ninja make"; exit 1; }
	build/release/test/unittest '$(PROJ_DIR)test/sql/integration/*' 2>&1 | tee build/integration.log
	scripts/ci/assert_ran.sh build/integration.log 1 1 'require-env MSSQL_DUCKLAKE_TEST_DSN'

.PHONY: vcpkg-setup
vcpkg-setup:
	@test -d vcpkg || git clone https://github.com/microsoft/vcpkg.git vcpkg
	./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# ci-tools' tidy-check configures cmake with the toolchain and the merged-manifest flags but depends on
# neither; the code-quality workflow that runs it sets up no vcpkg. Give the target what the flags
# assume (a bootstrapped vcpkg, the merged vcpkg.json) so `make tidy-check` works there and here.
tidy-check: vcpkg-setup $(EXTENSION_CONFIG_STEP)
