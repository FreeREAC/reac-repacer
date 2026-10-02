# Building reac-repacer

How to build the packages and run the tests from a source checkout. To install
and run the daemon, see the [README](README.md); for how it works and how to tune
it, [docs/internals.md](docs/internals.md).

## OpenWrt packages

Builds an OpenWrt `.apk` against the latest stable OpenWrt SDK, in a container
(podman or docker — nothing else needed on the host):

    ./scripts/build.sh                          # latest stable, mediatek/filogic (e.g. GL-MT6000)
    OPENWRT_RELEASE=24.10.2 ./scripts/build.sh  # pin a release
    OPENWRT_TARGET=ramips/mt7621 ./scripts/build.sh
    LIBREAC_REF=v0.4.0 ./scripts/build.sh       # pin the libreac it builds against

The apks land in `.build/out/`; the SDK is downloaded once and cached.

## The libreac dependency

The one build dependency is [libreac](https://github.com/FreeREAC/libreac)
(≥ 0.4.0), the shared REAC wire-format core: the frame geometry (`52 + n × 36`, the
same law in both directions and at every sample rate), the `+2` length rule and the
channel-pair braid oracle used by the test-only inject paths. Those two extra bytes are
the low 16 bits of the frame's **own Ethernet FCS**, left behind by some capture paths —
not a protocol field, and not OHRCA-specific despite the legacy name of libreac's
`REAC_FRAME_BYTES_OHRCA` constant. `reac_frame_clean_len()` strips them; nothing may
ever emit them. The relay itself does not decode REAC and needs none of it. The build
script clones libreac and stages its OpenWrt recipe alongside this one, so
nothing extra is needed on the host; install libreac on the device from its own
release. `reac-transport` is a separate matter — a *runtime* pairing, not a
dependency.

## Native build + tests

    meson setup build && meson test -C build

Uses the system `libreac-devel` when it is new enough, otherwise clones libreac
as a meson subproject. The tests cover the frame geometry and pin the bytes the
inject paths write against a golden fixture; the relay path is exercised on the
rig, not here.
