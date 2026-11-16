#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""The daemon has one way to name a port and one name for each flag.

`--port IN:OUT` is the only port form, and `--forward-only` the only name of the
forward-only switch. An earlier `--in IFACE --out IFACE` pair and `--no-return`
are refused like any other unknown option: exit 1, the ordinary message, and the
daemon never starts. Parsing happens before any socket is opened, so the refusal
needs no privileges and no interface.

usage: cli_refusals.py PATH-TO-reac-repacer
"""
import subprocess
import sys

REFUSED = [
    ["--in", "nosuch0", "--out", "nosuch1"],
    ["--no-return", "--port", "nosuch0:nosuch1"],
]


def main(binary):
    bad = 0
    for args in REFUSED:
        try:
            r = subprocess.run([binary] + args, capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            print("FAIL %s: the daemon started instead of refusing" % " ".join(args))
            bad += 1
            continue
        ok = r.returncode == 1 and "unknown or malformed option '%s'" % args[0] in r.stderr
        print("%s %s: exit %d" % ("ok  " if ok else "FAIL", " ".join(args), r.returncode))
        if not ok:
            print(r.stderr)
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
