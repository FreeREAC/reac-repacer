# reac-repacer

Transparent Layer-2 **de-jitter / re-pacing relay** for a Roland **REAC**
(EtherType `0x8819`) audio stream carried over a bursty Wi-Fi / WDS link —
packaged for OpenWrt, with a LuCI app.

Part of [FreeREAC](https://github.com/FreeREAC) — *REAC Exposed Audio
Communications*.

## What it does

A REAC stagebox is a clock slave: it expects a frame at a fixed cadence (a frame
every 125 µs at 96 kHz, 250 µs at 48 kHz, 272 µs at 44.1 kHz) and clicks if that
cadence stalls. Wi-Fi delivers in bursts. `reac-repacer` sits at
the receiving end, buffers the master broadcast a few milliseconds, and re-emits
a constant cadence on a recovered clock to the local stagebox — so the stagebox
stays locked. One daemon paces every REAC port on a single shared clock, so the
boxes stay sample-aligned. Latency is moved by clock rate, never by dropping
frames, so a latency change does not click — but the shipped profile
(`servo_clamp_ppm=0`) **freezes** the output clock and holds latency where the
prefill put it. Raising `servo_clamp_ppm` is what lets the buffer drain toward
the link's clean floor; see [docs/internals.md](docs/internals.md).

It does **not** decode the REAC audio payload, reorder frames, or tag VLANs — it
relays whole L2 frames. It does touch two things in the header: it re-stamps the
16-bit frame counter (bytes 14–15) so its own output is one monotonic sequence,
and it reads bytes 16–17 to tell a control frame from an audio frame. With gap
concealment on (the default) it also *synthesises* a frame on underrun, a repeat
of the last one under the next counter. The VLAN trunk + gretap fabric is the separate
[reac-transport](https://github.com/FreeREAC/reac-transport) package; install
both when the path crosses Wi-Fi.

## Sample rates

REAC carries no rate field on the wire — the sample rate *is* the packet rate
(`pps = rate / 12`: 3675 pps at 44.1 kHz, 4000 at 48 kHz, 8000 at 96 kHz).
`reac-repacer` **auto-detects** the rate by measuring the packet cadence on the wired
side and paces its output at exactly that period (`1e9 / pps` ns). It tracks the live
rate, so switching the console 44.1 ↔ 48 ↔ 96 kHz re-locks the relay on the fly; the
frame is rate-invariant, so nothing else about the relay changes. The detection window
is the `detect_ms` UCI option. For the underlying REAC clock model, see
[reac-protocol](https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md).

## Build

Builds an OpenWrt `.apk` against the latest stable OpenWrt SDK, in a container
(podman or docker — nothing else needed on the host):

    ./scripts/build.sh                          # latest stable, mediatek/filogic (e.g. GL-MT6000)
    OPENWRT_RELEASE=24.10.2 ./scripts/build.sh  # pin a release
    OPENWRT_TARGET=ramips/mt7621 ./scripts/build.sh
    LIBREAC_REF=v0.4.0 ./scripts/build.sh       # pin the libreac it builds against

The apks land in `.build/out/`; the SDK is downloaded once and cached.

The one build dependency is [libreac](https://github.com/FreeREAC/libreac)
(≥ 0.4.0), the shared REAC wire-format core: the frame geometry, the OHRCA `+2`
trailer rule and the channel-pair braid oracle used by the test-only inject
paths. The relay itself does not decode REAC and needs none of it. The build
script clones libreac and stages its OpenWrt recipe alongside this one, so
nothing extra is needed on the host; install libreac on the device from its own
release. `reac-transport` is a separate matter — a *runtime* pairing, not a
dependency.

### Native build + tests

    meson setup build && meson test -C build

Uses the system `libreac-devel` when it is new enough, otherwise clones libreac
as a meson subproject. The tests cover the frame geometry and pin the bytes the
inject paths write against a golden fixture; the relay path is exercised on the
rig, not here.

## Install

    apk add ./reac-repacer-*.apk
    apk add ./luci-app-reac-repacer-*.apk   # optional web UI
    /etc/init.d/reac-repacer enable
    /etc/init.d/reac-repacer start

## Configure

Edit `/etc/config/reac-repacer` (section `main`) or use the LuCI page
(*Services → REAC Wi-Fi Re-pacer*). The directional AP/STA profile is auto-filled
at install from the box's Wi-Fi role. Full option reference: `man reac-repacer`; how
the buffer, recovered clock, PLL/servo and interface binding work and how to tune
them: **[docs/internals.md](docs/internals.md)**.

Retune the running daemon without a restart:

    ubus call reac_repacer set '{"prefill_ms":150}'
    ubus call reac_repacer get

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
