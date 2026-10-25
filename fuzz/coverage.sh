#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE COVERAGE WITNESS (`make fuzz-coverage`). Each harness, built for llvm-cov without
# sanitizers, runs once over its seed corpus (-runs=0 executes the corpus and nothing
# else); this prints the line coverage of the parser functions the harness exists to
# drive, and every line of them the corpus never reached. A file's other functions
# (the other harness's readers) are not the harness's job and are not counted.
#
# THE FLOOR. fuzz/coverage-floor.txt holds, per harness, the number of covered parser
# lines the committed corpus reaches. A harness whose covered lines fall below its floor
# turns this red: a seed lost, a harness that stopped driving a function, or a parser
# that grew lines its corpus never reaches. Raise a floor when coverage rises; lower one
# only together with the change that removed the lines.
#
# THE HARNESS FLOOR. Every property check of the harness itself, each abort() in
# fuzz/fuzz_<harness>.c and the statement that guards it (fuzz/checks.awk), must be
# executed by at least one seed. A check no seed reaches is a property nobody tests,
# and the library's line count cannot see it; this names the harness file and the
# line of every such check and turns red.
#
# THE RECORD RATCHET. Every check names in its comment a breakage that it catches
# (`mutant:` and `input:`), or says in one `not a property: <reason>` line why it is
# none; a check with neither turns this red. fuzz/records.sh proves the records.
#
# Usage: coverage.sh <cov build dir> <harness>...   (LLVM_COV names llvm-cov)
set -eu
B=$1
shift
LLVM_COV=${LLVM_COV:-llvm-cov}
FLOOR=fuzz/coverage-floor.txt
[ -f "$FLOOR" ] || { echo "coverage.sh: no $FLOOR" >&2; exit 2; }
low=
dead=
norec=

# The functions each harness covers: what "every parser line the harness means to
# cover" means, one list per harness. Static functions are matched by bare name.
funcs() {
	case $1 in
	wire) echo src/repace_wire.c :: is_reac is_tagged_reac is_audio rp_fwd_class rp_ret_accept \
		rp_ds_accept rp_meter_counter rp_counter rp_frame_channels rp_copy_untag rp_slot_width \
		rp_slot_read ;;
	egress) echo src/repace_wire.c :: is_audio rp_frame_channels rp_slot_width rp_slot_write \
		rp_restamp rp_mute ;;
	lock) echo src/repace_conf.c :: std_period is_sep field digits rp_lock_lookup ;;
	uci) echo src/repace_conf.c :: rp_uci_int rp_hot_clamp ;;
	*) echo "coverage.sh: no function list for harness '$1'" >&2; exit 2 ;;
	esac
}

for h in "$@"; do
	set -- $(funcs "$h")
	files=
	while [ "$1" != "::" ]; do files="$files $1"; shift; done
	shift
	bin=$B/fuzz_$h prof=$B/$h.profdata
	echo "== fuzz_$h: $(ls fuzz/corpus/$h | wc -l) seed inputs"
	# Name Regions Miss Cover Lines Miss Cover ...: keep the listed functions.
	tot=$B/$h.covered
	"$LLVM_COV" report -show-functions "$bin" -instr-profile="$prof" $files |
	awk -v want="$*" -v tot="$tot" '
		BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) keep[w[i]] = 1 }
		{ name = $1; sub(/^.*:/, "", name) }
		(name in keep) && NF >= 10 {
			printf "  %-34s lines %4d  missed %3d  %s\n", name, $5, $6, $7
			lines += $5; missed += $6; seen[name] = 1
		}
		END {
			for (k in keep) if (!(k in seen)) printf "  %-34s NOT FOUND in the report\n", k
			printf "  %-34s lines %4d  missed %3d  %.2f%%\n", "TOTAL", lines, missed,
			       lines ? 100 * (lines - missed) / lines : 0
			print lines - missed > tot
		}'
	for f in "$@"; do
		"$LLVM_COV" show "$bin" -instr-profile="$prof" -name-regex="^(.*:)?$f\$" $files \
			2>/dev/null |
		awk -v f="$f" -F'|' '$2 ~ /^ *0$/ {
			sub(/^ */, "", $1); printf "    unreached %s:%s %s\n", f, $1, $3 }'
	done
	src=fuzz/fuzz_$h.c
	"$LLVM_COV" show "$bin" -instr-profile="$prof" "$src" |
		awk -v file="$src" -f fuzz/checks.awk >"$B/$h.checks"
	nchk=$(wc -l <"$B/$h.checks")
	if [ "$nchk" -eq 0 ]; then
		echo "  CHECKS: no property check found in $src"
		dead="$dead $h"
	elif awk -F: '$4 == "0" { n++ } END { exit !n }' "$B/$h.checks"; then
		awk -F: '$4 == "0" { t = $0; sub(/^([^:]*:){5}/, "", t)
			printf "  CHECK: %s:%d no seed reaches (abort at line %d): %s\n", $1, $2, $3, t }' \
			"$B/$h.checks"
		dead="$dead $h"
	else
		echo "  checks: all $nchk property checks of $src reached"
	fi
	if awk -F: '$5 == "0" || $5 == "bad" { n++ } END { exit !n }' "$B/$h.checks"; then
		awk -F: '$5 == "0" || $5 == "bad" { t = $0; sub(/^([^:]*:){5}/, "", t)
			printf "  RECORD: %s:%d (abort at line %d) names no breakage and is not marked not a property: %s\n",
			       $1, $2, $3, t }' "$B/$h.checks"
		norec="$norec $h"
	fi
	covered=$(cat "$tot")
	floor=$(awk -v h="$h" '$1 == h { print $2 }' "$FLOOR")
	if [ -z "$floor" ]; then
		echo "  FLOOR: fuzz_$h has no line in $FLOOR"
		low="$low $h"
	elif [ "$covered" -lt "$floor" ]; then
		echo "  FLOOR: fuzz_$h covers $covered parser lines, below its floor of $floor"
		low="$low $h"
	else
		echo "  floor $floor: held ($covered covered)"
	fi
done

[ -z "$low" ] || echo "make fuzz-coverage: RED -- below the coverage floor:$low"
[ -z "$dead" ] || echo "make fuzz-coverage: RED -- harness checks no seed reaches:$dead"
[ -z "$norec" ] || echo "make fuzz-coverage: RED -- harness checks with no mutant record:$norec"
[ -z "$low$dead$norec" ] || exit 1
echo "make fuzz-coverage: every harness at or above its coverage floor, every check reached and recorded"
