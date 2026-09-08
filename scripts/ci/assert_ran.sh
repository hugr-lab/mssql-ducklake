#!/usr/bin/env bash
# The floor under a green test step: a suite that ran nothing, or skipped the
# files that mattered, must not pass. sqllogictest marks a `require`/`require-env` miss as *skipped*
# rather than failed, and every e2e script exits 0 on a missing dependency by design - both are
# right on a developer box and wrong on a CI runner that was set up precisely to run them.
#
#   scripts/ci/assert_ran.sh <log> <min_test_cases> <min_assertions> [forbidden-skip-regex]
#
# Reads the unittest summary line "All tests passed (N skipped tests, A assertions in C test cases)"
# (singular "assertion"/"test case" when the count is 1 - a one-file suite is what test-integration runs)
# from <log>, requires C >= min_test_cases and A >= min_assertions, and fails if any line of the
# "Skipped tests for the following reasons:" block matches the regex (e.g. `require-env MSSQL_DUCKLAKE_TEST_DSN`
# on a job that provides SQL Server). Also fails on any `SKIP:` line - the e2e scripts' own signal.
set -euo pipefail
log="$1"; min_cases="$2"; min_assertions="$3"; forbidden="${4:-}"
[ -f "$log" ] || { echo "assert_ran: no log at $log" >&2; exit 1; }

if grep -qE '^SKIP:' "$log"; then
	echo "assert_ran: a script skipped itself on this runner:" >&2
	grep -E '^SKIP:' "$log" >&2
	exit 1
fi

if [ "$min_cases" -eq 0 ] && [ "$min_assertions" -eq 0 ]; then
	exit 0 # an e2e log: the SKIP check above is the whole floor
fi
# Catch pluralises both words: "1 assertion in 1 test case" on a one-file, one-statement run
summary="$(grep -oE '[0-9]+ assertions? in [0-9]+ test cases?' "$log" | tail -1 || true)"
if [ -z "$summary" ]; then
	# every file skipped: duckdb's Catch prints "All tests were skipped (total skipped N)" and no
	# assertion summary - name the skip reason (the forbidden one if it matches) rather than asking
	# whether the suite ran
	if grep -q 'All tests were skipped' "$log"; then
		skipped="$(sed -n '/Skipped tests for the following reasons:/,$p' "$log" | grep -v 'Skipped tests' || true)"
		if [ -n "$forbidden" ] && grep -qE "$forbidden" <<<"$skipped"; then
			echo "assert_ran: a skip this runner must not have:" >&2
		else
			echo "assert_ran: every test file skipped itself:" >&2
		fi
		echo "$skipped" >&2
		exit 1
	fi
	# a failed suite prints a different summary ("test cases: N | M passed | K failed"); say so rather
	# than asking whether it ran at all
	failed="$(grep -oE 'test cases:\s+[0-9]+\s+\|\s+[0-9]+ passed\s+\|\s+[0-9]+ failed' "$log" | tail -1 || true)"
	if [ -n "$failed" ]; then
		echo "assert_ran: the suite failed - $failed (the failures are above)" >&2
		exit 1
	fi
	echo "assert_ran: no unittest summary line in $log - did the suite run at all?" >&2
	exit 1
fi
assertions="$(awk '{print $1}' <<<"$summary")"
cases="$(awk '{print $4}' <<<"$summary")"
if [ "$cases" -lt "$min_cases" ] || [ "$assertions" -lt "$min_assertions" ]; then
	echo "assert_ran: $summary - expected at least $min_cases test cases and $min_assertions assertions" >&2
	exit 1
fi

if [ -n "$forbidden" ]; then
	skipped="$(sed -n '/Skipped tests for the following reasons:/,$p' "$log" | grep -E "$forbidden" || true)"
	if [ -n "$skipped" ]; then
		echo "assert_ran: a skip this runner must not have:" >&2
		echo "$skipped" >&2
		exit 1
	fi
fi
echo "assert_ran: $summary"
