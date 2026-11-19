#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# Refuses a Fedora 44 reference in the workflows: the fc44 packages are dropped for this pass (protocol 0.6.0 and later
# are published for fc45 only), so a job on fedora:44, an fc44 cell, or 44 in a fedora matrix list can no longer resolve
# its build dependencies. Matches fedora:44, fc44, a fedora line carrying 44 anywhere (fedora: [45, 44], [43, 44]), and a
# fedora block list whose item is 44. Usage: workflow-fedora-check.sh [repo-root] (default: .), or --self-test, which runs
# this check on planted trees with each shape, a missing directory included. Exit 0 clean, 1 a hit (each line named),
# 2 no .github/workflows to check.
set -eu
self=$0

check() {
	local wf=$1/.github/workflows
	if [ ! -d "$wf" ]; then
		echo "workflow-fedora-check: no $wf to check (refusing: an empty check proves nothing)" >&2
		return 2
	fi
	local hits
	hits=$(grep -rnE 'fedora:44|fc44|fedora.*[^0-9]44([^0-9]|$)' "$wf" || true)
	local blocks
	blocks=$(find "$wf" -type f \( -name '*.yml' -o -name '*.yaml' \) -exec awk '
		FNR == 1 { fed = 0 }
		/fedora:[[:space:]]*$/ { fed = FNR; next }
		fed && FNR - fed <= 6 && /^[[:space:]]*-[[:space:]]*[^0-9]?44[^0-9]?[[:space:]]*$/ { print FILENAME ":" FNR ": fedora block list item 44" }
	' {} + || true)
	if [ -n "$hits$blocks" ]; then
		[ -n "$hits" ] && printf '%s\n' "$hits"
		[ -n "$blocks" ] && printf '%s\n' "$blocks"
		echo "workflow-fedora-check: Fedora 44 reference in .github/workflows (move it to fedora:45 / fc45)" >&2
		return 1
	fi
	echo "workflow-fedora-check: no Fedora 44 reference in .github/workflows"
}

if [ "${1:-}" = "--self-test" ]; then
	t=$(mktemp -d)
	trap 'rm -rf "$t"' EXIT
	fails=0
	plant() { # name, expected rc, yaml body
		local d=$t/$1
		mkdir -p "$d/.github/workflows"
		printf '%s\n' "$3" > "$d/.github/workflows/ci.yml"
		local rc=0
		"$self" "$d" >/dev/null 2>&1 || rc=$?
		if [ "$rc" != "$2" ]; then echo "self-test FAIL $1: rc $rc, want $2" >&2; fails=$((fails + 1)); fi
	}
	plant clean 0 'jobs:
  a:
    container:
      image: fedora:45'
	plant image44 1 'image: fedora:44'
	plant fc44name 1 'name: omx (fc44, x86_64)'
	plant matrix44last 1 'fedora: [45, 44]'
	plant matrix44mid 1 'fedora: [43, 44, 45]'
	plant blocklist 1 'fedora:
  - 45
  - 44'
	plant blocklistfirst 1 'fedora:
  - 44
  - 45'
	plant not144 0 'fedora: [45, 144]'
	mkdir -p "$t/nodir"
	rc=0; "$self" "$t/nodir" >/dev/null 2>&1 || rc=$?
	if [ "$rc" != 2 ]; then echo "self-test FAIL missing dir: rc $rc, want 2" >&2; fails=$((fails + 1)); fi
	if [ "$fails" != 0 ]; then echo "workflow-fedora-check: self-test $fails red" >&2; exit 1; fi
	echo "workflow-fedora-check: self-test green (10 planted trees)"
	exit 0
fi

rc=0
check "${1:-.}" || rc=$?
exit "$rc"
