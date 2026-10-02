#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The README sells the package: what it is, how to install and configure it. Build
# steps live in BUILDING.md. This fails when a build command shows up in the
# README's command text -- indented or fenced code blocks and inline code spans --
# so prose ("make sure", "a meson subproject") never trips it.
#
#   readme_no_build.py README.md     exit 1 naming each offending line
#   readme_no_build.py --self-test   prove it catches what it claims to

import re
import sys

BUILD = re.compile(r"(?<![\w./-])(make|meson|cmake|ninja|rpmbuild|pnpm\s+build|\S*build(-apk)?\.sh)(?![\w-])")
SPAN = re.compile(r"`([^`]+)`")


def command_text(text):
    """Yield (line number, command text) for every code line and inline code span."""
    fenced = False
    for n, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced or line.startswith(("    ", "\t")):
            yield n, line
        else:
            for span in SPAN.findall(line):
                yield n, span


def offenders(text):
    return [(n, code.strip()) for n, code in command_text(text) if BUILD.search(code)]


def self_test():
    red = [
        "    meson setup build && meson test -C build\n",
        "    ./scripts/build.sh\n",
        "    OPENWRT_RELEASE=24.10.2 ./scripts/build.sh\n",
        "```sh\nmake -C src\n```\n",
        "Run `rpmbuild -ba reac-repacer.spec` first.\n",
        "Then `pnpm build`.\n",
        "\tcmake -B out\n",
    ]
    green = [
        "    apk add ./reac-repacer-*.apk\n",
        "    ubus call reac_repacer set '{\"prefill_ms\":150}'\n",
        "Make sure libreac is installed; it builds as a meson subproject.\n",
        "See [BUILDING.md](BUILDING.md) to build from source.\n",
        "Edit `/etc/config/reac-repacer` (section `main`).\n",
    ]
    bad = [t for t in red if not offenders(t)] + [t for t in green if offenders(t)]
    for t in bad:
        print("self-test: misjudged %r" % t, file=sys.stderr)
    print("readme-no-build self-test: %d red, %d green, %d misjudged" % (len(red), len(green), len(bad)))
    return 1 if bad else 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print("usage: readme_no_build.py README.md | --self-test", file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        found = offenders(f.read())
    for n, code in found:
        print("%s:%d: build command in the README (it belongs in BUILDING.md): %s" % (argv[1], n, code))
    if found:
        return 1
    print("%s: no build command" % argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
