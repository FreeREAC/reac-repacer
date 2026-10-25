# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PROPERTY CHECKS OF ONE HARNESS SOURCE (fuzz/coverage.sh, fuzz/records.sh).
# Reads `llvm-cov show` of the source (`LINE|COUNT|source`), or with -v plain=1 the
# source itself, and prints, for every property check in it, one line
#
#   <file>:<check line>:<abort line>:<count>:<records>:<the check's first source line>
#
# A property check is an abort() and the statement that guards it: the `if` whose
# condition ends in the abort, or, for a bare abort() in a block (an `else` arm, a
# switch's `default:`), the statement that opens that block. <count> is how often the
# seed corpus executed that statement's first line (empty with plain=1); 0 is a
# property no seed tests.
#
# <records> is what the check's comment documents: the number of its `mutant:` and
# `input:` pairs, `np` for a `not a property: <reason>` line, `bad` for a record
# that does not read as one. 0 is a property with no breakage named: THE RATCHET,
# which coverage.sh and records.sh refuse. The check's comment is what the OSS Verify
# record reader takes (its tools/check_records.py): the comment lines directly above
# the check's first line and directly above its abort, with no code and no empty line
# between, and those inside the check.
#
# With -v records=1 it prints instead one line per record, its fields separated by
# -v sep (a tab by default),
#
#   <file> <abort line> <path> <line> <old> <new> <input>
#
# and one `BAD <file> <abort line> <why>` line per record that does not read.
# Comments are stripped before code is matched, so an abort() named in prose is not
# a site.
BEGIN {
	if (!plain) FS = "|"
	if (sep == "") sep = "\t"
}
{
	if (plain) {
		ln = NR
		cnt = ""
		src = $0
	} else {
		# llvm-cov ends with an empty line, and may draw separators: no line number.
		if ($1 !~ /^ *[0-9]+ *$/) next
		ln = $1 + 0
		cnt = $2
		gsub(/[ \t]/, "", cnt)
		src = $0
		sub(/^[^|]*\|[^|]*\|/, "", src)
	}
	code = src
	cm = ""
	if (incomment) {
		if (code ~ /\*\//) {
			cm = code
			sub(/\*\/.*$/, "", cm)
			sub(/^.*\*\//, "", code)
			incomment = 0
		} else {
			cm = code
			code = ""
		}
	}
	while (match(code, /\/\*([^*]|\*+[^*\/])*\*+\//)) {
		cm = cm " " substr(code, RSTART + 2, RLENGTH - 4)
		code = substr(code, 1, RSTART - 1) substr(code, RSTART + RLENGTH)
	}
	if (code ~ /\/\*/) {
		t = code
		sub(/^.*\/\*/, "", t)
		cm = cm " " t
		sub(/\/\*.*$/, "", code)
		incomment = 1
	}
	if (match(code, /\/\//)) {
		cm = cm " " substr(code, RSTART + 2)
		code = substr(code, 1, RSTART - 1)
	}
	gsub(/^[ \t]+|[ \t]+$/, "", code)
	C[ln] = code; N[ln] = cnt; S[ln] = src
	CM[ln] = clean(cm)
	HAS[ln] = cm != ""
	if (code ~ /(^|[^A-Za-z0-9_])abort\(\);/) site[++ns] = ln
	if (ln > last) last = ln
}

# A comment line without its markers: the leading spaces and asterisks, and a
# trailing `*/` already went with the comment delimiters.
function clean(t) {
	sub(/^[ \t*]+/, "", t)
	sub(/[ \t*]+$/, "", t)
	return t
}

# The first line of the statement that ends on line l.
function stmt_start(l) {
	while (l > 1 && C[l - 1] != "" && C[l - 1] !~ /[;{}:]$/) l--
	return l
}

# Marks in IN[] the comment lines directly above line l: no code, no empty line.
function above(l) {
	for (l--; l >= 1 && HAS[l] && C[l] == ""; l--) IN[l] = 1
}

# Reads the records of the comment lines marked in IN[] for the check whose abort is
# on line a; returns the count, "np" or "bad", and with records=1 prints them.
function records_of(a,    l, t, n, np, bad, pend, rest, path, line, old, new, k) {
	n = 0; np = 0; bad = 0; pend = 0
	for (l = 1; l <= last; l++) {
		if (!IN[l]) continue
		t = CM[l]
		if (pend) {
			pend = 0
			if (t ~ /^input:[ \t]*[^ \t]+$/) {
				sub(/^input:[ \t]*/, "", t)
				n++
				if (records)
					print file sep a sep path sep line sep old sep new sep t
				continue
			}
			bad = 1
			if (records)
				print "BAD" sep file sep a sep "no input: line after the mutant at " path ":" line
		}
		if (t ~ /^mutant:/) {
			rest = t
			sub(/^mutant:[ \t]*/, "", rest)
			k = index(rest, " ")
			path = substr(rest, 1, k - 1)
			rest = substr(rest, k + 1)
			line = path
			sub(/^.*:/, "", line)
			sub(/:[^:]*$/, "", path)
			if (line !~ /^[0-9]+$/ || rest !~ /^`[^`]+`[ \t]*->[ \t]*`[^`]*`$/) {
				bad = 1
				if (records) print "BAD" sep file sep a sep "unreadable: " t
				continue
			}
			old = rest
			sub(/^`/, "", old)
			sub(/`.*$/, "", old)
			new = rest
			sub(/^`[^`]*`[ \t]*->[ \t]*`/, "", new)
			sub(/`$/, "", new)
			pend = 1
		} else if (t ~ /^input:/) {
			bad = 1
			if (records) print "BAD" sep file sep a sep "an input: line with no mutant: line"
		} else if (t ~ /^not a property:[ \t]*[^ \t]/) {
			np = 1
		}
	}
	if (pend) {
		bad = 1
		if (records)
			print "BAD" sep file sep a sep "no input: line after the mutant at " path ":" line
	}
	if (bad || (np && n)) return "bad"
	return np ? "np" : n
}

END {
	for (i = 1; i <= ns; i++) {
		l = site[i]
		s = stmt_start(l)
		bare = s == l && C[l] ~ /^abort\(\);/
		if (bare) {
			d = 0
			for (s = l - 1; s >= 1 && d >= 0; s--)
				for (k = length(C[s]); k >= 1; k--) {
					c = substr(C[s], k, 1)
					if (c == "}") d++
					else if (c == "{" && --d < 0) break
				}
			s = stmt_start(s + 1)
		}
		split("", IN)
		above(s)
		above(l)
		# Inside the check: the lines of its `if`. A bare abort()'s opener is a
		# whole block, whose comments belong to the checks inside it.
		if (!bare)
			for (k = s; k <= l; k++) if (HAS[k]) IN[k] = 1
		r = records_of(l)
		if (records) continue
		t = S[s]
		gsub(/^[ \t]+/, "", t)
		printf "%s:%d:%d:%s:%s:%s\n", file, s, l, N[s], r, t
	}
}
