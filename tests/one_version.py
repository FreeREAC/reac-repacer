#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# One release, one version. The daemon apk, the LuCI apk and the Fedora package are
# cut from the same tag, so the meson project and every recipe state the same version. Release 0.2.3 shipped
# luci-app-reac-repacer 0.2.1 beside reac-repacer 0.2.3 because the LuCI recipe was
# never bumped; this fails when the statements differ.
#
#   one_version.py [ROOT]      exit 1 naming each statement
#   one_version.py --self-test prove it catches what it claims to

import os
import re
import sys
import tempfile

STATEMENTS = (
    ("meson.build", re.compile(r"^\s*version\s*:\s*'([^']+)'", re.M)),
    ("packaging/reac-repacer.spec", re.compile(r"^Version:\s*(\S+)\s*$", re.M)),
    ("openwrt/reac-repacer/Makefile", re.compile(r"^PKG_VERSION:=(\S+)\s*$", re.M)),
    ("openwrt/luci-app-reac-repacer/Makefile", re.compile(r"^PKG_VERSION:=(\S+)\s*$", re.M)),
)


def versions(root):
    found = {}
    for path, rx in STATEMENTS:
        with open(os.path.join(root, path), encoding="utf-8") as f:
            m = rx.search(f.read())
        found[path] = m.group(1) if m else None
    return found


def check(root):
    found = versions(root)
    if None not in found.values() and len(set(found.values())) == 1:
        print(f"one version: {next(iter(found.values()))}")
        return 0
    for path, v in found.items():
        print(f"{path}: {v if v else 'no version statement'}", file=sys.stderr)
    print("the packages of one release must state one version", file=sys.stderr)
    return 1


def self_test():
    def tree(meson, spec, daemon, luci):
        d = tempfile.mkdtemp()
        for (path, _), text in zip(STATEMENTS, (f"project('x', 'c',\n  version : '{meson}',\n",
                                                f"Version:        {spec}\n",
                                                f"PKG_VERSION:={daemon}\n",
                                                f"PKG_VERSION:={luci}\n")):
            os.makedirs(os.path.dirname(os.path.join(d, path)), exist_ok=True)
            with open(os.path.join(d, path), "w", encoding="utf-8") as f:
                f.write(text)
        return d

    ok = check(tree("0.2.4", "0.2.4", "0.2.4", "0.2.4")) == 0
    ok &= check(tree("0.2.3", "0.2.3", "0.2.3", "0.2.1")) == 1
    ok &= check(tree("0.2.4", "0.2.3", "0.2.3", "0.2.3")) == 1
    print("self-test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    sys.exit(check(sys.argv[1] if len(sys.argv) > 1 else "."))
