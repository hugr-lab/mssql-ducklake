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

.PHONY: vcpkg-setup
vcpkg-setup:
	@test -d vcpkg || git clone https://github.com/microsoft/vcpkg.git vcpkg
	./vcpkg/bootstrap-vcpkg.sh -disableMetrics
