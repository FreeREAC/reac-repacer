#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a reac-repacer source tarball for rpmbuild (one C program over libreac,
# which the spec pulls in as libreac-devel). Writes reac-repacer-<version>.tar.gz
# to the repo root.
set -e
V="${1:-0.2.3}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); D="$T/reac-repacer-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' --exclude 'subprojects/libreac' \
      "$ROOT/tools" "$ROOT/openwrt" "$ROOT/tests" "$ROOT/subprojects" "$ROOT/meson.build" \
      "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/BUILDING.md" "$ROOT/packaging" "$D/"
tar -czf "$ROOT/reac-repacer-$V.tar.gz" -C "$T" "reac-repacer-$V"
rm -rf "$T"
echo "wrote $ROOT/reac-repacer-$V.tar.gz"
