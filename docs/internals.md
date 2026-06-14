# How reac-repacer works — internals and tuning

What the daemon actually does to a REAC stream, and how each parameter shapes the
buffer, the recovered clock, the PLL/servo, and the interface binding. For the REAC
clock model this builds on, see
[reac-protocol](https://github.com/FreeREAC/reac-protocol/blob/main/wire-format.md);
for the option-by-option reference, `man reac-repacer`.

## The problem

A REAC stagebox is a hardware clock slave with **no jitter buffer**: it recovers its
word clock from the *arrival cadence* of the master broadcast (one frame per slot —
125 µs at 96 kHz, 250 µs at 48 kHz, 272 µs at 44.1 kHz). A late frame is a late
sample. Carried over Wi-Fi/WDS the stream arrives in bursts, so the stagebox hears
cadence jitter as clicks and lock-flap.

`reac-repacer` sits between the bursty link and the stagebox and **re-imposes the
exact cadence the slave expects**, on a clean recovered clock. It does not decode
REAC, reorder or alter bytes, or tag VLANs — it relays whole Layer-2 frames.

## The de-jitter pipeline (per port)

```
 tunnel side (bursty)                                      stagebox side (clean)
   IN iface  ──▶  ring buffer  ──▶  recovered-clock pacer  ──▶  OUT iface
                (absorbs bursts)    (one frame per slot)        (ETF egress)
```

1. **Ingest.** Frames arriving on the IN interface are pushed into a per-port ring
   buffer. A burst just raises the ring occupancy; it never reaches the output.
2. **Pace.** A single real-time (`SCHED_FIFO`) thread wakes once per slot and emits
   one frame per active port from its ring. The wake period is the **recovered base
   period** (`1e9 / pps` ns), free-running — not modulated per tick.
3. **Egress.** The frame is sent raw on the OUT interface, optionally with kernel
   time-based TX (ETF / `SO_TXTIME`) so the on-wire instant is hardware-timed rather
   than left to scheduler wake jitter.

The **return** direction (stagebox → tunnel) is passed straight through: the master
owns the clock and tolerates input jitter, so it needs no de-jittering.

One daemon drives every REAC port of one mixer on **one shared pacing clock**, so the
stageboxes stay sample-phase-aligned with each other (mirroring the mixer's single
word clock).

## The recovered output clock

There is no rate field on the wire, so the daemon **measures** the rate: it counts
REAC frames on the wired side over a window (`detect_ms`) and divides by elapsed time
→ pps → `rate = pps × 12`. The base emit period is `1e9 / pps` ns. The wire is a
crystal, so averaging the frame count over seconds recovers that crystal to sub-ppm.

By default the output clock is **steady and free-running** (`--no-steady` turns this
off): the emit period runs at the detected base period and is *not* nudged per tick or
per occupancy swing. This is what keeps short-term bursts out of the cadence — they
show up as ring-depth swings, never as emit-timing jitter. (`clock_source` selects
where the base reference comes from — the local crystal vs. the wired in-port frame
count; the directional profile sets it per role, below.)

## The PLL / drain servo

Two slow mechanisms keep the free-running clock correct without touching short-term
cadence:

- **PLL (`pll`)** — a *glacial* frequency lock. Integrated over seconds, it nudges the
  base period **sub-ppm** to null the long-term occupancy drift (the small constant
  offset between the master's crystal and the router's). It takes minutes to converge;
  warm-start (below) skips that wait. `clock_margin_ppm` is its lock margin.
- **Drain servo (`servo_clamp_ppm`)** — bounds how fast *latency* is reclaimed. The
  buffer is allowed to drain toward its floor by applying a **small, clamped clock
  bias**, never by dropping frames. `servo_clamp_ppm` is the ceiling on that bias:
  - `0` (the shipped value) = **frozen output clock** — latency is held; only the PLL
    trims. Click-free and the validated default.
  - higher = reclaim latency faster. ~700 ppm (≈1 cent) is inaudible; raise for
    quicker latency reduction, lower for a wider inaudibility margin.

The governing principle: **reclaim latency by clock rate, never by dropping frames.**
A target-depth change walks the occupancy to the new depth with a bounded bias, so
audio stays continuous and only the latency moves. `--reclaim` (off by default) would
actively shrink latency and *can* click; the default is grow-only.

## How a real stagebox locks — and where the re-pacer differs

Comparing the re-pacer to a hardware REAC stagebox sharpens what it does well and
where it can still improve. A real stagebox is a **synchronous TDM clock slave**: it
doesn't just match the master's *rate*, it **phase-locks to the master's slot grid**.
Each downstream frame defines a slot boundary, and the box answers its upstream a
**fixed offset after that boundary** — its whole timebase is hung off the arrival
edge of the master broadcast, so it sits at a constant phase within every slot, not
just at the right average frequency. (Observed on the wire: the box's return cadence
tracks the master's downstream cadence frame-for-frame, with no buffer of its own.)

The re-pacer, by contrast, **rate-locks**: it recovers the master's *rate* from the
arrival cadence (see above) and runs a clean free-running pacer at that rate, while a
buffer absorbs the burstiness the wire adds. It only **optionally** phase-aligns —
`pace_by_downstream` (the AP/master-side profile) nudges the emit instant toward the
downstream occupancy, but the steady default is a free-running emit clock whose phase
relative to the desk's slot grid is left wherever it started.

So the rate side of the problem is well covered (the PLL nulls long-term drift to
sub-ppm). The **biggest remaining improvement is phase**: drive the emit instant to a
fixed offset from the desk's own slot boundaries — i.e. track *when* in each slot the
master would have placed the frame — rather than only nulling long-term rate drift on
a free-running phase. That is what would make the relayed cadence indistinguishable
from a real box to the slave's recovery loop, not merely the same average rate.
**[inferred]** — the phase-lock win is reasoned from the hardware's behaviour, not yet
measured as an audible improvement on the rig.

What the comparison also **validates** is the architecture itself. A hardware stagebox
of the slave/splitter class does exactly the shape the re-pacer does: it is a
**clock-slave on its input, re-driven as clock-master on its output**, with an
**elastic buffer** between the two halves and a **per-frame counter re-stamp** on the
frames it re-emits. The re-pacer is the same pipeline — recover the input cadence,
buffer, re-impose a clean cadence, rewrite the per-frame counter on egress — so the
re-pacer's design is the software expression of a pattern Roland's own hardware uses,
which is reassuring evidence the approach is sound. The difference is only in how
tightly each side closes the phase loop, not in the overall shape.

## The buffer

- **`prefill_ms`** — the target buffer depth, filled before emitting begins, and the
  depth the servo walks toward. Deeper = more burst tolerance, more latency.
- **Adaptive sizing (`adapt`)** — size the buffer to the *observed* burst depth: grow
  at once to cover a burst, ease down when the link is calm. Bounded by `adapt_min_ms`
  (floor) and `adapt_max_ms` (ceiling), with `adapt_margin` slots of headroom kept
  above the occupancy low-water mark.

These are **hot** parameters: a `ubus set` or a Save & Apply retunes them on the next
pacing tick with no dropout.

## Warm-start

A glacial PLL lock takes minutes; a cold start spends those minutes audibly
converging. Warm-start (on by default) persists the converged per-port emit period to
a small lock file and seeds the base period from it at startup, so the daemon starts
**already locked**. The lock is keyed per output interface and per detected rate label,
so a router restores only its own ports, and only when the saved rate matches the rate
detected at boot. A missing or stale file is tolerated — it just cold-starts.

## Interface binding

- **`port` = `IN:OUT[:etf_µs]`** — one REAC zone per entry: tunnel-side iface :
  stagebox-side iface, optionally a per-port ETF window. Repeat per zone.
- **Unbridged interfaces** — the relay needs raw Layer-2 TX, which a VLAN-filtering
  bridge slave drops, so the relay ports are taken out of the bridge. The kernel's
  `.11`/`.12`/`.13` VLAN sub-interfaces still tag the tunnel side; the relay ports
  themselves are untagged.
- **ETF egress (`etf`, `etf_delta_us`)** — install an `sch_etf` qdisc on the OUT port
  so the daemon's `SO_TXTIME` timestamps actually time egress (hardware launch-time on
  a capable NIC), instead of leaving frames at `sendmsg` time with scheduler wake
  jitter. `etf_delta_us` is the early-release window. This is the master-side fix
  (below).

## The directional AP/STA profile

The two halves of a Wi-Fi rig de-jitter **opposite directions** and need different
parameters. `/etc/uci-defaults/97-reac-role` fills the profile from the box's Wi-Fi
role at install:

| param | AP / master side | STA / box side |
|---|---|---|
| `clock_source` | `local` | `local-in` |
| `pll` | 1 | 0 |
| `pace_by_downstream` | 1 | 0 |
| `etf` | 1 | 0 |
| `bcast_only` | 0 | 1 |

- **AP / master side** feeds the **phase-strict mixer master**, which resamples the
  emit cadence straight into audio — so it must be hard-timed: PLL on, ETF on,
  `pace_by_downstream` driving the clock from the downstream occupancy.
- **STA / box side** feeds **PLL-tolerant stageboxes**, which recover their own clock
  and tolerate a looser cadence — so it runs lighter: no ETF, broadcast-only.

## The hardware story (why ETF matters)

The master side's quality is gated by how precisely egress can be timed. The real fix
is a NIC with **TSN hardware launch-time** — Intel **i226**-class (i210/i225/i226):
the `igc` driver offloads `SO_TXTIME` so each frame leaves at a hardware-scheduled
instant, plus a PTP hardware clock. On a switch/SoC GMAC (e.g. the mt7531 in a
GL-MT6000) there is **no offload**, so ETF falls back to software timing — better than
nothing, but it still carries scheduler jitter. This is the master-clock timing limit:
the box side is fine on commodity hardware; the master side wants TSN. See the findings
in [reac-docs](https://github.com/FreeREAC).

## Live retuning

- **Hot** (no restart, no dropout): `prefill_ms`, `adapt_min_ms`, `adapt_max_ms`,
  `adapt_margin`, `servo_clamp_ppm` — via `ubus call reac_repacer set '{...}'`, or a
  Save & Apply / `uci commit reac-repacer` which fires the procd reload trigger
  (SIGHUP). Omitted options keep their running value.
- **Structural** (restart-only): the `port` / interface mapping and the pinned `cpu`.
  A live change to these is rejected, not silently ignored.

Inspect the running state with `ubus call reac_repacer get` (target depth, base
period, per-port occupancy and counters, whether an occupancy retarget is in progress).

## Parameter reference

| UCI option | CLI flag | hot? | what it does |
|---|---|---|---|
| `enabled` | — | — | master on/off |
| `wait_iface` | — | — | gate: don't launch until this fabric iface exists |
| `port` (list) | `--port IN:OUT[:µs]` | no | REAC zone(s); IN = tunnel, OUT = stagebox |
| `prefill_ms` | `--prefill-ms` | **yes** | target buffer depth (ms) |
| `servo_clamp_ppm` | `--servo-clamp-ppm` | **yes** | latency-reclaim clock-bias clamp; 0 = frozen clock |
| `clock_margin_ppm` | — | no | PLL lock margin (ppm) |
| `detect_ms` | `--detect-ms` | no | rate-detection window (ms) |
| `pll` | `--pll` | no | glacial frequency lock (null long-term drift) |
| `clock_source` | `--clock-source` | no | base-clock reference (`local` / `local-in`) |
| `pace_by_downstream` | `--pace-by-downstream` | no | drive the clock from downstream occupancy (AP) |
| `forward_only` | `--forward-only` | no | de-jitter forward only; no return thread |
| `bcast_only` | `--bcast-only` | no | accept only master-broadcast frames |
| `etf` | `--etf` | no | kernel time-based TX egress (needs the qdisc) |
| `etf_delta_us` | (per-port `:µs`) | no | ETF early-release window |
| `cpu` | `--cpu` | no | core for the `SCHED_FIFO` pacing thread |
| `adapt` | `--adapt` | (flag) | auto-size the buffer to burst depth |
| `adapt_min_ms` / `adapt_max_ms` / `adapt_margin` | same | **yes** | adaptive-window floor / ceiling / headroom |
| `role` | — | — | fill marker (`ap`/`sta`) set by 97-reac-role |

The shipped UCI defaults are the ear-validated rig values; the CLI defaults (in
`man reac-repacer`) are the fallbacks when a flag is omitted.
