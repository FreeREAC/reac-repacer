# REPACER-FIXES.md

Prioritized fix list from reviewing `reac-repacer` (`tools/reac_repacer.c`,
`pacer_probe.c`, `docs/internals.md`) and reac-pw's `reac_pacer.c` against the 5
establishment/timing findings, plus the FSM code the pacer drives
(`reac_master.c/.h`, `reac_fsm.c/.h`, `reac_slave.c`) and the FSM spec
(`reac-firmware-re/REAC-CONNECTION-FSM.md`). READ-ONLY; the rig was not touched.

## Verified already-correct (no change needed)
- **Finding #1** — `reac_pacer.c` `pacer_loop` (`src/reac_pacer.c:537-585`) emits
  exactly ONE frame per slot, re-stamps a monotonic counter via
  `reac_master_next`, and never inserts an extra frame at counter+1; the
  guard-trim (`:528`) drops OLDEST audio but keeps counters contiguous.
- **Finding #2 (MASTER side)** — `link_check` decrements once per wire slot
  (`reac_master.c:1004`) and reloads on every box RX (`:837`), fps-scaled to
  ~6.5 s (`:637`).
- **Finding #4** — DMX-style periodic full-state re-assert is implemented in
  `reac_master.c` control_cadence: the master advertises the full control set
  continuously in both PROBING and ESTABLISHED, one frame per slot.

The real gaps are in the SLAVE timers and the re-pacer hygiene, below.

---

## P1 (rig-gated) — Slave establishment timers are fixed frame-counts, not rate-scaled
**File:** `reac-pw/src/reac_fsm.h:35,41,49,55,69`
(`LINKCHECK_RELOAD 600`, `TXMUTE_DWELL 800`, `FLOOD_BURST 5460`,
`JOIN_RETRY_PERIOD 800`, `GRANT_ACK_FRAMES 7200`).

**Current:** all five establishment timers are FIXED frame-counts. Only
`heartbeat_period` is rate-scaled (`reac_slave.c:77` = `sample_rate/12`). The
doc comments even self-flag `FLOOD_BURST` scaling as "rig-TBD" and `JOIN_RETRY`
as "~100ms @8000fps".

**Corrected:** rate-scale each from fps at init exactly as the master already
does (`reac_master.c:606-639`: `cycle_len`, `probe_stride`, `grant_frames`,
`grant_dwell`, `link_check_reload` are all `fps*const/base`).
`JOIN_RETRY_PERIOD` and `TXMUTE_DWELL` = `fps/10` (~100 ms); `GRANT_ACK_FRAMES`
= 9× that grid; `FLOOD_BURST` = `~1.36s*fps`; `LINKCHECK_RELOAD` stays a
wire-frame count (600) but should be derived so intent is explicit. Store the
resolved counts on the fsm struct (like `heartbeat_period`), not `#define`s.

**Why:** at 48k the 800-frame grid becomes 200 ms (2× intended) and at 44.1k
218 ms; the 7200 ACK window becomes 1.8 s; `FLOOD_BURST` 5460 becomes 0.68 s
@96k (half the byte-verified 1.36 s). Because heartbeat is scaled but these are
not, the slave's establishment cadence is only faithful at exactly
96k/8000fps. At 48k the 800-frame TX_MUTE dwell (200 ms) also exceeds a real
master's ~150 ms grant window (`fps*15/100`), the exact constraint
`reac_fsm.h:37-40` warns about. The #156 96k-OHRCA path made rate a live
variable, so this asymmetry is now reachable.
*(The scaling mechanism itself is offline-testable; the exact resolved constants
are rig-gated.)*

## P2 (offline-safe) — Slave link-check budget is not a clean wire-frame count
**File:** `reac-pw/src/reac_fsm.c:181-182` (re-arm) + `:199` (decrement).

**Current:** in `FSM_ESTABLISHED` the `link_check` countdown is RE-ARMED only on
`rx->kind == MASTER_HB || MASTER_ANNOUNCE`, and DECREMENTED only in the TICK
branch (`:199`). Master filler RX neither re-arms nor decrements. The RX-clocked
slave engine (`reac_slave.c`) only issues `FSM_EV_TICK` on an idle poll, so the
budget is effectively "consecutive master-silent idle slots", not a clean
wire-frame count.

**Corrected:** make the budget a true wire-frame count that mirrors the master
(`reac_master.c:1004` decrements every slot, `:837` reloads on EVERY box RX
event): decrement `link_check` once per ESTABLISHED wire slot, and reload on ANY
received master downstream frame (filler included), not just HB/announce. This
is the doc's stated preference (`REAC-CONNECTION-FSM.md` "Timers/budgets":
"prefer a wire-clocked tick").

**Why:** the doc calls the current per-received-event decrement "a known
fidelity gap, wrong at 96k". Reloading only on HB/announce (~1–2/s) means a
master that streams mostly filler between sparse HBs relies on ticks staying
rare to avoid a false PEER_GONE drop; that invariant is implicit and
rate-dependent. Reloading on any master frame + decrementing per slot makes the
600-frame budget mean the same 75 ms @96k / 150 ms @48k the FPGA enforces, and
matches the master side exactly. Offline-testable via the existing FSM-replay
harness (doc "Offline validation" step 3).

## P3 (rig-gated) — Clock-meter comment mislabels the mechanism — **FIXED**
**File:** `reac-repacer/tools/reac_repacer.c:381` (comment) + `:461-498`
(`clk_meter`).

**Fixed** by option (a): comment-only, clock path untouched. `g_met_n` and the
cumulative-fit block now name the wire-counter delta, and both carry the reason
raw socket counting must not come back (it undercounts on drops; it paced an
M-5000 4800 ppm slow).

**Current:** `g_met_n` is commented "driver-level rx_packets of the OUT iface",
but `clk_meter` actually reads the REAC frame COUNTER field (bytes 14-15) off an
`AF_PACKET` socket and accumulates counter deltas — it never reads the NIC
driver's `rx_packets` from sysfs.

**Corrected:** either (a) reconcile the comment to state it is a drop-immune
wire-counter delta (not `rx_packets`), or (b) switch the count to the driver's
`rx_packets` per the pinned operator truth. Do NOT change the clock path without
restating the operator's settled algorithm first (per
`feedback_reac_repacer_clock_truth`).

**Why:** the PINNED note mandates "the NIC driver `rx_packets`, counted before
any socket" precisely because userspace packet counting starved the M-5000 at
4800 ppm slow. The current counter-delta is drop-immune by a DIFFERENT valid
mechanism (a dropped frame doesn't lower the counter), so it likely satisfies
the intent — but the comment misrepresents it as `rx_packets`, which will
mislead the next clock-path edit. Reconcile before anyone "fixes" it back to raw
socket counting.

## P4 (offline-safe) — `--ctrl-bypass` violates one-frame-per-slot
**File:** `reac-repacer/tools/reac_repacer.c:427-433` (`--ctrl-bypass` path).

**Current:** with `--ctrl-bypass`, a control frame (`is_ctrl`) is `sendto()`'d
immediately AND `continue`d past the ring — so it is emitted AHEAD of the
buffered audio cadence and removed from the counter sequence: an extra frame out
of slot order.

**Corrected:** keep it as a clearly-labelled A/B-only debug flag (it already
is), but document at the flag site that it deliberately violates the
one-frame-per-slot rule and must never ship enabled; or remove it now that the
artifact it reproduces is understood.

**Why:** finding #1 ("control frames REPLACE the audio/flood frame at that slot,
never an extra frame at counter+1") plus
`reference_reac_repacer_link_status_vs_audio` ("DO NOT fast-path control frames
— reorders the isochronous stream, breaks word-clock recovery"). The default
in-sequence path is already correct (every emitted frame is re-stamped
monotonically at `:703` and flows through the ring); `--ctrl-bypass` is the one
path that reproduces the forbidden two-frames-per-slot shape.

## P5 (offline-safe) — usage() advertises a flag that doesn't exist — **FIXED**
**File:** `reac-repacer/tools/reac_repacer.c:1061` (usage) vs `:1115` (parse).

**Fixed** as proposed, plus a named rejection: `--clock-margin-ms` now exits with
a message pointing at `--clock-margin-ppm` rather than a bare "unknown option".

**Current:** `usage()` advertises
`--clock-margin-ms N  local mode: buffer movement (ms)...`, but the parser
accepts `--clock-margin-ppm` and the value drives `g_clock_margin_ppm` (a ppm
re-derive threshold, default 2). The advertised flag does not exist and the real
one is undocumented.

**Corrected:** fix the usage string to
`--clock-margin-ppm N  cumulative-rate change (ppm) that re-applies the wire clock (default 2)`.

**Why:** the clock-margin is the trigger that re-applies the cumulative
frames/elapsed estimate (the operator-settled clock). An operator copying the
advertised `--clock-margin-ms` gets an "unknown option" hard-exit (the parser
rejects unknown flags at `:1129-1135`) and cannot discover the real ppm knob.
Trivial offline doc fix.

## P6 (rig-gated) — Re-pacer gap budget must be re-validated against the 96k ceiling
**File:** `reac-repacer/tools/reac_repacer.c` (prefill/PLC defaults) —
cross-cutting with the reac-pw link-check budget.

**Current:** underrun tolerance, prefill depth and PLC were ear-validated at 96k
on the M-5000 rig, but the shipped CLI default prefill is 12 ms and the rig
configs (70–100 ms) were tuned when 48k dominated. The link-check budget is a
WIRE-frame count: 600 frames = 75 ms @96k but 150 ms @48k.

**Corrected:** re-validate that no re-pacer-introduced gap (startup settle,
relock, sustained underrun re-prefill) exceeds ~75 ms at 96k — size the
gap/recovery budget against the 96k link-check ceiling, not the 48k one. The
steady-clock + PLC + no-re-prefill design already targets this; confirm the
margin holds at doubled cadence (#156 OHRCA).

**Why:** the same 600-frame budget the slave/master enforce halves in wall-clock
at 96k. `reference_reac_repacer_link_status_vs_audio` proved that gaps (not
latency) blow the >3-missed-tick watchdog → stuck "linking". At 96k a gap that
was safe at 48k can now trip it. Validation/tuning item, not a code defect.

## P7 (offline-safe, test-only) — Test sine injector hardcodes the sample rate
**File:** `reac-repacer/tools/reac_repacer.c:373` (`inj_sine`).

**Current:** the injector hardcodes the rate:
`g_inj_ph += 2.0*M_PI*g_inj_freq/96000.0`.

**Corrected:** derive the divisor from the detected rate
(`rate_label_hz(period_ns)`) so `--inject-sine` produces the requested frequency
at 48k/44.1k too.

**Why:** at 48k the injected tone comes out 2× the requested pitch, which would
confound any A/B delivery-path listening test done at 48k. Offline, test-only,
low impact.

---

## Priority / gating summary
| P  | File:line | Offline-safe? |
|----|-----------|---------------|
| P1 | `reac_fsm.h:35,41,49,55,69` | mechanism yes; exact constants rig-gated |
| P2 | `reac_fsm.c:181-182,199` | yes |
| P3 | `reac_repacer.c:381,461-498` | rig-gated (clock path) |
| P4 | `reac_repacer.c:427-433` | yes |
| P5 | `reac_repacer.c:1061,1115` | yes |
| P6 | `reac_repacer.c` prefill/PLC | rig-gated (validation) |
| P7 | `reac_repacer.c:373` | yes (test-only) |

Do first, safely: P2, P4, P5, P7 (and the scaling MECHANISM of P1).
Rig-gated for exact values: P1 constants, P3, P6.