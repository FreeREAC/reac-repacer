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

## When this is the right tool — and when it is not

The job here is narrow and physical: **a hardware stagebox at the far end of a link that
delivers in bursts.** A REAC slave recovers word clock from packet arrival cadence and
has no jitter buffer, so it cannot absorb a Wi-Fi burst itself. Nothing else in the
fabric can do that job for it — the cadence has to be re-imposed on the segment the box
is on, by something sitting on that segment.

It is **not** the tool for a rate or pace mismatch inside a host. A mixing graph running
at one rate with a REAC wire at another is fixed in the host's audio adapter, where
PipeWire's own resampler bridges the two paces (verified at a 192 kHz graph against a
48 kHz wire). Putting a relay in that path adds a hop and fixes nothing. Likewise a host
whose pacer drifts is fixed in its pacer — see the deadline rule in
[docs/internals.md](docs/internals.md) — not by re-pacing its output afterwards.

So: **wired gigabit path → you do not need this.** Wi-Fi, WDS, or any link that bursts,
with real Roland boxes downstream → you do.

**[?] Open, and it bounds how much of this daemon's tuning was ever necessary.** A host
pacing an adapter at the wrong sample rate can discard ~100 ms of audio at a time, which
sounds exactly like a bursty link — and can be in the path at the same time as a real
Wi-Fi burst, so a symptom fixed by tuning this relay is not proof the link caused it.
The two causes stay separable only by checking different counters: on the **host**, a
guard-trim / dropped-frame counter that should read zero once the host's own adapter is
paced correctly (a sustained trim there means the host, not the link, is the cause); on
the **link**, this relay's own upstream gap counter, which counts frames that genuinely
never arrived. A host that trims and a link that drops sound alike and are fixed in
different places — check both before tuning this daemon further.

## Sample rates

REAC carries no rate field on the wire — the sample rate *is* the packet rate
(`pps = rate / 12`: 3675 pps at 44.1 kHz, 4000 at 48 kHz, 8000 at 96 kHz).
`reac-repacer` **auto-detects** the rate by measuring the packet cadence on the wired
side and paces its output at exactly that period (`1e9 / pps` ns). It tracks the live
rate, so switching the console 44.1 ↔ 48 ↔ 96 kHz re-locks the relay on the fly; the
frame is rate-invariant, so nothing else about the relay changes. The detection window
is the `detect_ms` UCI option. For the underlying REAC clock model, see
[reac-protocol](https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md).

## Install

    apk add ./reac-repacer-*.apk
    apk add ./luci-app-reac-repacer-*.apk   # optional web UI
    /etc/init.d/reac-repacer enable
    /etc/init.d/reac-repacer start

The package depends on [libreac](https://github.com/FreeREAC/libreac) (≥ 0.4.0);
install it on the device from its own release first. To build the apks or run the
tests from a source checkout, see [BUILDING.md](BUILDING.md).

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
