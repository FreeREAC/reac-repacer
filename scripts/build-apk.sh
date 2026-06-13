#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# In-container build of the reac-aes67 OpenWrt packages (.apk) against the LATEST
# stable OpenWrt release (or a pinned one). Invoked by build/build.sh, which mounts
# the repo read-only at /repo and a persistent work/cache dir at /work.
#
#   OPENWRT_RELEASE   pin a release (e.g. 24.10.2); default = latest stable
#   OPENWRT_TARGET    target board path (default mediatek/filogic = GL-MT6000)
#
# Produces: reac-aes67, luci-app-reac-aes67, reac-repacer, luci-app-reac-repacer,
# reac-rig .apk in /work/out. The SDK is downloaded once and cached in /work.
set -euo pipefail

REPO=/repo
WORK=/work
OUT_DIR="$WORK/out"
TARGET="${OPENWRT_TARGET:-mediatek/filogic}"
BASE="https://downloads.openwrt.org/releases"

echo "== build deps (RPM) =="
dnf -q -y install gcc gcc-c++ make git python3 ncurses-devel zlib-devel \
    zstd xz wget curl which unzip file rsync perl gawk diffutils findutils \
    tar bzip2 patch >/dev/null
echo "deps installed"

# ---- resolve the OpenWrt release (latest stable unless pinned) --------------
REL="${OPENWRT_RELEASE:-}"
if [ -z "$REL" ]; then
    echo "== resolving latest stable OpenWrt release =="
    REL="$(curl -fsSL "$BASE/" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+/' | tr -d '/' \
        | sort -V | tail -1)"
    [ -n "$REL" ] || { echo "ERROR: could not resolve latest OpenWrt release from $BASE/"; exit 1; }
fi
echo "OpenWrt release: $REL   target: $TARGET"

# ---- locate + cache the target SDK tarball (name varies by toolchain) -------
TGT_URL="$BASE/$REL/targets/$TARGET"
SDK_FILE="$(curl -fsSL "$TGT_URL/" | grep -oE 'openwrt-sdk-[^"<]*\.tar\.(xz|zst)' | head -1)"
[ -n "$SDK_FILE" ] || { echo "ERROR: no SDK tarball found at $TGT_URL/"; exit 1; }
mkdir -p "$WORK/sdk-cache"
SDK_CACHE="$WORK/sdk-cache/$SDK_FILE"
if [ ! -f "$SDK_CACHE" ]; then
    echo "== download SDK: $SDK_FILE =="
    curl -fSL --retry 3 -o "$SDK_CACHE.part" "$TGT_URL/$SDK_FILE"
    mv "$SDK_CACHE.part" "$SDK_CACHE"
fi

# ---- unpack the SDK (fresh dir per release+target) --------------------------
SDK_DIR="$WORK/sdk-$REL-$(echo "$TARGET" | tr '/' '-')"
if [ ! -d "$SDK_DIR" ]; then
    echo "== unpack SDK -> $SDK_DIR =="
    mkdir -p "$SDK_DIR"
    case "$SDK_FILE" in
        *.tar.zst) tar --use-compress-program=unzstd -xf "$SDK_CACHE" -C "$SDK_DIR" --strip-components=1 ;;
        *.tar.xz)  tar -xJf "$SDK_CACHE" -C "$SDK_DIR" --strip-components=1 ;;
        *) echo "ERROR: unknown SDK compression: $SDK_FILE"; exit 1 ;;
    esac
fi
cd "$SDK_DIR"
echo "SDK: $(cat version 2>/dev/null || echo "$REL")"

# ---- feeds (first run for this SDK dir) -------------------------------------
if [ ! -d feeds/luci ]; then
    echo "== feeds =="
    cp feeds.conf.default feeds.conf
    ./scripts/feeds update -a >/tmp/feeds-update.log 2>&1 || { tail -20 /tmp/feeds-update.log; exit 1; }
    ./scripts/feeds install luci-base >/dev/null 2>&1 || true
fi

echo "== stage our packages from /repo =="
# reac-aes67 daemon: recipe + vendored src (repo top-level src/) + files
mkdir -p package/reac-aes67/src package/reac-aes67/files
cp "$REPO/openwrt/reac-aes67/Makefile" package/reac-aes67/Makefile
cp "$REPO"/src/*.c "$REPO"/src/*.h package/reac-aes67/src/
cp "$REPO"/openwrt/files/reac-aes67.config "$REPO"/openwrt/files/reac-aes67.init \
   "$REPO"/openwrt/files/capabilities-reac-aes67.json package/reac-aes67/files/
mkdir -p package/luci-app-reac-aes67
cp -r "$REPO"/openwrt/luci-app-reac-aes67/* package/luci-app-reac-aes67/

# reac-repacer: recipe + vendored single src (the SHIPPING source is tools/
# reac_repacer.c — NOT openwrt/reac-repacer/src/, which is a build artifact). The
# committed source keeps RING_BITS=14 for host test headroom; PROD ships 11.
mkdir -p package/reac-repacer/src package/reac-repacer/files
cp "$REPO/openwrt/reac-repacer/Makefile" package/reac-repacer/Makefile
cp "$REPO"/tools/reac_repacer.c package/reac-repacer/src/reac_repacer.c
sed -i 's/^#define RING_BITS 14 .*/#define RING_BITS 11   \/* PROD ring (patched from test=14 by build-apk.sh) *\//' \
    package/reac-repacer/src/reac_repacer.c
grep -q '^#define RING_BITS 11' package/reac-repacer/src/reac_repacer.c \
    || { echo "ERROR: failed to patch RING_BITS to 11 in vendored re-pacer src"; exit 1; }
cp "$REPO"/openwrt/files/reac-repacer.config "$REPO"/openwrt/files/reac-repacer.init package/reac-repacer/files/
cp "$REPO"/openwrt/reac-repacer/files/reac-repacer.8 package/reac-repacer/files/
mkdir -p package/luci-app-reac-repacer
cp -r "$REPO"/openwrt/luci-app-reac-repacer/* package/luci-app-reac-repacer/

# reac-rig: declarative REAC-over-WDS fabric + re-pacer launcher (pure integration:
# config + init + nftables.d + uci-defaults; no source to compile).
mkdir -p package/reac-rig
cp "$REPO/openwrt/reac-rig/Makefile" package/reac-rig/Makefile
cp -r "$REPO"/openwrt/reac-rig/files package/reac-rig/files

echo "== configure =="
make defconfig >/dev/null 2>&1
{ echo "CONFIG_PACKAGE_reac-aes67=y"; echo "CONFIG_PACKAGE_luci-app-reac-aes67=y"; \
  echo "CONFIG_PACKAGE_reac-repacer=y"; echo "CONFIG_PACKAGE_luci-app-reac-repacer=y"; \
  echo "CONFIG_PACKAGE_reac-rig=y"; } >> .config
make defconfig >/dev/null 2>&1

for pkg in reac-aes67 luci-app-reac-aes67 reac-repacer luci-app-reac-repacer reac-rig; do
    echo "== compile $pkg =="
    make "package/$pkg/compile" V=s -j"$(nproc)" 2>&1 | tee "/tmp/build-$pkg.log" | tail -5 || {
        echo "--- $pkg build tail ---"; tail -40 "/tmp/build-$pkg.log"; exit 1; }
done

echo "== artifacts =="
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.apk
find bin -name '*.apk' \( -iname '*reac*' -o -iname '*luci-app-reac*' \) -exec cp {} "$OUT_DIR/" \;
echo "release $REL ($TARGET) — apks copied to $OUT_DIR:"
ls -la "$OUT_DIR/"
echo "$REL" > "$OUT_DIR/.openwrt-release"
