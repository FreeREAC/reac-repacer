# The host-side repacing defect, and why it may be ours too (2026-08-21)

Found while making a REAC segment run at a different rate from the mixing graph. It is a
HOST-side defect, not a Wi-Fi one — but the symptom is the same family this relay exists to
fix, so it is worth re-testing the Wi-Fi path against it before assuming the link is at fault.

## What it was

openmixer's REAC adapter (`reac-pw`) published its PipeWire nodes as **`pw_filter`s**. A
filter's ports are raw DSP ports at the **graph** rate with no `audioconvert` in the path, so
the nodes could only ever run at the graph's pace. `PW_KEY_NODE_RATE` looked like it declared
the wire rate, but on a filter it is only a REQUEST for the whole graph to switch — which a
graph driven by a hardware clock (an RME at 96 kHz here) refuses.

So with the wire at 48 kHz and the graph at 96 kHz, the node received 96 kHz worth of buffers
and made up the difference by DISCARDING frames from its own ring — about 100 ms at a time,
reported only as a rising counter. Audibly: granulated and saturated.

Measured, one run at a 48 kHz wire inside a 96 kHz graph:

    guard trims       1273
    frames dropped    364757

## The fix

Both nodes became **`pw_stream` adapters declaring the REAC rate in their FORMAT**, so
PipeWire's own resampler bridges the two paces. Same rig, same rates, audio both ways:

    guard trims       0
    frames dropped    0

Conversion belongs in the adapter and is done by the graph's resampler — not by the
application, and not by forcing either side to follow the other.
(openmixer `docs/design/specs/2026-08-21-reac-adapter-pace-and-port-contract.md`;
reac-pw tag `reac-repacing-through-pipewire-20260821`.)

## Why this repo should re-test

REAC over Wi-Fi has shown pacing trouble that this relay was built to absorb. Some of it may
have been the defect above rather than the link: a host discarding ~100 ms chunks produces
exactly the granular, saturated artefact a bursty link does, and both were in the path at
once. Before tuning the relay further, re-run the Wi-Fi path with the fixed host adapter and
see what is left.

**What to measure, so the two causes stay separable:**

- the **host** side: reac-pw's `guard trims` / `dropped` counters, which should now be 0. A
  sustained trim prints a loud `WIRE/GRAPH RATE MISMATCH` line naming the cause.
- the **link** side: the box's own upstream counter gaps (`reac_rx: gaps=`), which count
  frames that never arrived. Those are the link's, and this relay's business.

A host that trims and a link that drops sound alike and are fixed in different places.
