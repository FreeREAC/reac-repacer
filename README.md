# reac-repacer

Transparent Layer-2 **de-jitter / re-pacing relay** for a Roland **REAC**
(EtherType `0x8819`) audio stream carried over a bursty Wi-Fi / WDS link —
packaged for OpenWrt, with a LuCI app.

Part of [FreeREAC](https://github.com/FreeREAC) — *REAC Exposed Audio
Communications*.

## What it does

A REAC stagebox is a clock slave: it expects a frame roughly every 125 µs and
clicks if the cadence stalls. Wi-Fi delivers in bursts. `reac-repacer` sits at
the receiving end, buffers the master broadcast a few milliseconds, and re-emits
a constant cadence on a recovered clock to the local stagebox — so the stagebox
stays locked. One daemon paces every REAC port on a single shared clock, so the
boxes stay sample-aligned. Latency self-tunes down toward the link's clean floor
by clock rate alone; frames are never dropped, so reducing latency does not
click.

It does **not** decode REAC, reorder bytes, or tag VLANs — it relays whole L2
frames. The VLAN trunk + gretap fabric is the separate
[reac-transport](https://github.com/FreeREAC/reac-transport) package; install
both when the path crosses Wi-Fi.

## Build

Builds an OpenWrt `.apk` against the latest stable OpenWrt SDK, in a container
(podman or docker — nothing else needed on the host):

    ./scripts/build.sh                          # latest stable, mediatek/filogic (e.g. GL-MT6000)
    OPENWRT_RELEASE=24.10.2 ./scripts/build.sh  # pin a release
    OPENWRT_TARGET=ramips/mt7621 ./scripts/build.sh

The apks land in `.build/out/`; the SDK is downloaded once and cached.

## Install

    apk add ./reac-repacer-*.apk
    apk add ./luci-app-reac-repacer-*.apk   # optional web UI
    /etc/init.d/reac-repacer enable
    /etc/init.d/reac-repacer start

## Configure

Edit `/etc/config/reac-repacer` (section `main`) or use the LuCI page
(*Services → REAC Wi-Fi Re-pacer*). The directional AP/STA profile is auto-filled
at install from the box's Wi-Fi role. Full option reference: `man reac-repacer`.

Retune the running daemon without a restart:

    ubus call reac_repacer set '{"prefill_ms":150}'
    ubus call reac_repacer get

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
