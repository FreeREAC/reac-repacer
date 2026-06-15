#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a reac-repacer source tarball for rpmbuild (single self-contained C
# program, no libreac). Writes reac-repacer-<version>.tar.gz to the repo root.
set -e
V="${1:-0.2.3}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); D="$T/reac-repacer-$V"; mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' \
      "$ROOT/tools" "$ROOT/openwrt" "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/packaging" "$D/"
tar -czf "$ROOT/reac-repacer-$V.tar.gz" -C "$T" "reac-repacer-$V"
rm -rf "$T"
echo "wrote $ROOT/reac-repacer-$V.tar.gz"
