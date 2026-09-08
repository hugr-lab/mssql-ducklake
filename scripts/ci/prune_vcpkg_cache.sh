#!/usr/bin/env bash
# vcpkg's binary cache only ever grows: every rebuilt port adds an archive and
# nothing removes the old one, so the directory actions/cache saves grows until it eats the
# repository's 10 GB budget - at which point GitHub evicts OTHER caches by age, silently, and a
# 90-minute Arrow build comes back as a recurring cost. Drop archives untouched for two weeks before
# the cache is saved. Harmless where the directory does not exist.
set -euo pipefail
dir="${1:-vcpkg_bincache}"
if [ ! -d "$dir" ]; then
	echo "prune_vcpkg_cache: no $dir here - nothing to prune"
	exit 0
fi
before="$(du -sh "$dir" 2>/dev/null | cut -f1)"
find "$dir" -type f -mtime +14 -delete
echo "prune_vcpkg_cache: $dir $before -> $(du -sh "$dir" 2>/dev/null | cut -f1)"
