#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# `make fuzz-run`: every harness fuzzed for FUZZ_TIME seconds from its seed corpus,
# one after the other. Red when any harness crashes, leaks, times out or runs out of
# memory; the input that did it is left in fuzz/findings/<harness>-<kind>-<sha1>,
# and the harness's whole log beside it.
#
# New inputs the fuzzer finds go to a scratch corpus under the build directory,
# never into fuzz/corpus/, so a run leaves the tree as it found it.
#
# Usage: FUZZ_BUILD=build/fuzz FUZZ_TIME=60 run.sh <harness>...
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/${FUZZ_BUILD:-build/fuzz}
T=${FUZZ_TIME:-60}
FINDINGS=$ROOT/fuzz/findings
mkdir -p "$FINDINGS"

red=
for h in "$@"; do
	bin=$B/fuzz_$h
	work=$B/work/$h
	rm -rf "$work"
	mkdir -p "$work/new"
	echo "== fuzz_$h: ${T}s from fuzz/corpus/$h ($(ls "$ROOT/fuzz/corpus/$h" | wc -l) seeds)"
	# -jobs/-workers run the fuzzer as one child that logs to fuzz-0.log in the
	# working directory; the parent exits non-zero when the child found anything.
	(cd "$work" && "$bin" -max_total_time="$T" -max_len=1048576 -rss_limit_mb=2048 \
		-timeout=25 -jobs=1 -workers=1 -print_final_stats=1 \
		-artifact_prefix="$FINDINGS/$h-" new "$ROOT/fuzz/corpus/$h")
	rc=$?
	log=$work/fuzz-0.log
	if [ $rc -ne 0 ] || grep -qE '^SUMMARY: |ERROR: libFuzzer' "$log" 2>/dev/null; then
		cp "$log" "$FINDINGS/$h.log" 2>/dev/null
		sed -n '/ERROR: /,/^SUMMARY: /p' "$log"
		grep -E 'Test unit written to' "$log"
		echo "fuzz_$h: RED (exit $rc); input and log in fuzz/findings/"
		red="$red $h"
	else
		grep -E '^#[0-9]+[[:space:]]+DONE|stat::number_of_executed_units' "$log" | tail -2
		echo "fuzz_$h: no crash in ${T}s"
	fi
done

if [ -n "$red" ]; then
	echo "make fuzz-run: RED --$red"
	exit 1
fi
echo "make fuzz-run: every harness ran ${T}s without a crash"
