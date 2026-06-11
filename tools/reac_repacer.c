// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_repacer — protocol-agnostic L2 de-jitter / re-pacing relay (bidirectional,
// multiport).
//
// Sits between the gretap tunnel (Wi-Fi, bursty) and the local REAC stageboxes,
// with every interface unbridged so raw L2 TX works cleanly. Handles one or more
// REAC ports in a single daemon. Each port is an independent (in,out) pair:
//   forward (tunnel -> stagebox): buffers the bursty master broadcast a few ms
//      and re-emits a constant cadence — fills the gaps a clock-slave stagebox
//      cannot tolerate.
//   return  (stagebox -> tunnel): passes the stagebox's frames straight through
//      (the master is the clock owner and tolerates input jitter).
//
// Multiport sync: all ports of one mixer share a single word clock at the source,
// so they share ONE recovered pacing clock here — a single SCHED_FIFO tick emits
// every port on the same period, and the period is re-tuned from the active
// ports' average occupancy drift. Per-port jitter buffers, drops, holds and gap
// concealment keep a burst or underrun on one port from touching the others; only
// the slow clock is common (driven by the active ports), so a stalled port cannot
// drag the healthy ones. The result keeps the stageboxes sample-phase-aligned with
// each other, mirroring the mixer's own single master clock.
//
// It does NOT decode REAC, touch bytes/order, or do VLAN tagging — the kernel's
// .11/.12/.13 subinterfaces still tag the tunnel side; all relay interfaces are
// untagged. PACKET_OUTGOING frames are skipped on both RX paths so we never echo
// our own TX.
//
// Usage: reac_repacer --port reactap.11:lan1 --port reactap.12:lan2
//          --port reactap.13:lan3 [--prefill-ms 8] [--adapt] [--reclaim]
//          [--no-auto-rate] [--period-ns N] [--cpu 3] [--prio 80]
//          [--bcast-only] [--no-plc] [--ctrl-bypass] [--forward-only]
//          [--no-steady] [--no-warm-start] [--lockfile PATH]
//        (single port also accepts the legacy  --in IFACE --out IFACE  form.)
//        --help/-h prints the full usage and exits. At least one --port is
//        required: there is no implicit default, an unknown flag is rejected, and
//        a missing port is an error (the daemon never launches with defaults).
//
// --steady (default on; --no-steady to disable): pace the output from a FREE-RUNNING
//   clock at the base period, glacially rate-locked to the long-term input average via the
//   PLL (sub-ppm, seconds-integrated). The per-tick drain servo is kept OFF the emit path so
//   short-term WDS bursts are absorbed as occupancy swings and never reach the output cadence
//   -- the cure for the "rubbery on silences" cadence jitter. Occupancy is a slow trim only.
// --warm-start (default on; --no-warm-start to disable) + --lockfile PATH (default
//   /etc/reac-repacer.lock): persist the converged per-port emit period and, on startup, seed
//   the PLL from it so the daemon starts already-locked (no minutes-long audible convergence).
//
// --forward-only: de-jitter only the forward (IN->OUT) direction and do NOT run the
//   return pass-through thread. Used on the MIXER side (IN=tunnel, OUT=mixer): the
//   upstream box->master is de-jittered, while the downstream master->box is left on
//   the kernel bridge (lossless) instead of a starvable user-space pass-through.
//
// Live reconfiguration (no restart, no dropout): the "hot" tuning params -- target
// depth (prefill-ms / occupancy target), the adaptive window (min/max/margin) and the
// servo clamp -- can be retuned on a running daemon. Two triggers share ONE apply path:
//   ubus  reac_repacer set '{"prefill_ms":150}'   -- immediate, runtime-only, typed.
//   SIGHUP                                         -- re-read /etc/config/reac-repacer
//                                                     and apply the hot params (persists).
// A target-depth change does NOT flush/refill the ring: the occupancy is walked to the
// new target by a small, bounded clock bias held for a few hundred ms (grow = hold the
// output back, shrink = run it ahead), so audio stays continuous and only the latency
// walks to the new value. Structural params (the port/interface mapping) stay
// restart-only -- a live change there is rejected, not silently ignored.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#ifndef SO_TXTIME
#define SO_TXTIME 61
#endif
#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif
#ifndef SCM_TXTIME
#define SCM_TXTIME SO_TXTIME
#endif

#ifdef HAVE_UBUS
#include <libubus.h>
#include <libubox/blobmsg.h>
#endif

#ifndef PACKET_QDISC_BYPASS
#define PACKET_QDISC_BYPASS 20
#endif

#define RING_BITS 14   /* TEST: ~2 s ring so a 1 s passive buffer fits (prod = 11) */
#define RING_SZ   (1 << RING_BITS)
#define RING_MASK (RING_SZ - 1)
#define SLOT_SZ   1600
#define MAX_STREAMS 8

/* drain servo (latency control by clock rate, not by dropping frames) */
#define SERVO_SETPOINT 4.0   /* hold the tightest active port this many slots above its target */
#define SERVO_KP       8.0   /* proportional gain (ppm per slot) -- damps the loop */
#define SERVO_KI       0.02  /* integral gain -- nulls the residual rate offset (rate-match) */

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t got_hup = 0;   /* SIGHUP -> re-read UCI + apply hot params (set in the
                                             * handler only; the main loop does the work, so the
                                             * handler stays async-signal-safe) */

/* Live-reconfig request. A trigger (ubus set / SIGHUP) writes the new HOT params here
 * and bumps reconf_gen; the pacing loop notices the bump, clamps + applies them through
 * the one shared apply path (the glitch-free occupancy retarget for a depth change), and
 * publishes the applied values back for the ubus get reply. Plain ints written/read
 * word-atomically; reconf_gen is the single fence the loop polls. */
struct hot_params { int prefill_ms, adapt_min_ms, adapt_max_ms, adapt_margin, servo_clamp_ppm; };
static struct hot_params g_pending;                  /* staged by a trigger */
static volatile sig_atomic_t reconf_gen = 0;         /* bumped on each staged request */

/* global config (identical for every port) */
static int g_bcast_only, g_bypass, g_ctrl_bypass, g_auto_rate = 1;
static int g_etf, g_etf_lead_ns = 4000000;   /* --etf: emit via SCM_TXTIME; loose presubmit lead (ns) */
static long long g_cur_txtime;               /* this tick's ETF txtime (TAI ns); 0 => send immediately */
static long long g_tai_off;                  /* CLOCK_TAI - CLOCK_MONOTONIC (ns); sch_etf wants TAI */

/* tx: emit one frame on a PACKET socket. With --etf, stamp SCM_TXTIME = g_cur_txtime so the
 * kernel sch_etf qdisc releases it at exactly that instant (hardware-timer precision, no
 * SCHED_FIFO wake jitter); otherwise a plain immediate sendto. Returns like sendto. */
static ssize_t tx(int fd, const void *buf, size_t n, const struct sockaddr_ll *to) {
	if (!g_etf || !g_cur_txtime)
		return sendto(fd, buf, n, 0, (const struct sockaddr *)to, sizeof *to);
	struct iovec iov = { .iov_base = (void *)buf, .iov_len = n };
	char cbuf[CMSG_SPACE(sizeof(uint64_t))];
	struct msghdr msg = { .msg_name = (void *)to, .msg_namelen = sizeof *to,
	                      .msg_iov = &iov, .msg_iovlen = 1,
	                      .msg_control = cbuf, .msg_controllen = sizeof cbuf };
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_TXTIME;
	cm->cmsg_len = CMSG_LEN(sizeof(uint64_t));
	uint64_t t = (uint64_t)g_cur_txtime; memcpy(CMSG_DATA(cm), &t, sizeof t);
	return sendmsg(fd, &msg, 0);
}
static int g_forward_only;           /* de-jitter the forward (IN->OUT) only; leave the return
                                      * (OUT->IN) to the kernel bridge -- lossless, no pass-through
                                      * thread to starve. Used on the mixer side, where the
                                      * downstream (master->box) must stay on the bridge. */
static int g_servo_clamp_ppm = 700;    /* drain-servo clock-bias clamp (ppm). ~1.2 cents at 700 --
                                        * inaudible as a steady offset; bounds how fast latency is
                                        * reclaimed (latency falls ~ppm microseconds per second). */
static int g_adapt;                 /* adaptive variable-window: auto-size buffer to burst depth */
static int g_adapt_margin = 20;     /* keep the occ low-water-mark this many slots above 0 (safety) */
static int g_adapt_min_ms  = 6;     /* floor latency when shrinking */
static int g_adapt_max_ms  = 120;   /* ceiling latency when growing */
static int g_plc = 1;               /* gap concealment: repeat last frame (next counter) on underrun */
static int g_reclaim;               /* opt-in: actively shrink latency (drops -> clicks). Default grow-only. */
static int g_pll;                   /* glacial frequency-lock: converge BASE period to null occ drift */
/* The PLL: a HOLD-then-correct frequency lock. The emit period (= the stagebox's recovered
 * word clock) is held DEAD CONSTANT and re-trimmed ONLY when the buffer has drifted past a
 * frame threshold -- i.e. when the accumulated rate error has grown enough to matter. Between
 * trims the cadence does not move at all (no per-window occupancy-noise hunting, no periodic
 * step), so there is no audible rate modulation. Each trim nulls the *measured* rate error
 * (drift / elapsed ticks), clamped to PLL_MAXPPM so even the rare correction is inaudible. A
 * wide position band adds a gentle pull only if occ wanders far from target (rail safety over
 * a long show). Measuring drift over a long baseline (not per-window) is what lets a warm-
 * started, already-correct period simply never trip the trigger -> it just holds.
 * Runs with the fast servo OFF (--servo-clamp-ppm 0), preserving constant-rate clarity. */
#define PLL_WIN          32000  /* evaluation window in ticks (~4 s @ 8000 fps) */
#define PLL_FGAIN        0.6    /* fraction of the measured rate error nulled per correction */
#define PLL_TRIG_FRAMES  24.0   /* drift that triggers a correction: ~3 ms of buffer movement.
                                 * Well above per-window occupancy noise, far below any rail, so a
                                 * real rate error is caught while noise never trips it. At a 2-12 ppm
                                 * residual this fires only every ~4-25 min. */
#define PLL_POS_BAND     64.0   /* occ band (~8 ms) around target. INSIDE it the clock just holds + rate-
                                 * locks (inaudible). OUTSIDE it (a WDS-stall drain or cold start left occ
                                 * far from target) the loop adds a proportional occupancy RECOVERY and
                                 * widens the clamp so the buffer is steered back to a safe depth. */
#define PLL_RECOVER_SECS 60.0   /* off-band: steer occ back to target over ~this long (proportional, clamped
                                 * to PLL_ACQ_PPM). Gentle (~sub-cent) for a small excursion; ramps toward
                                 * the 500 ppm clamp only for a deep/dangerous drain -- rare, and beats a
                                 * rail. Rate-lock (the inaudible steady-state job) is unaffected. */
#define PLL_MAXPPM       30.0   /* LOCKED clamp: max single steady-state correction (ppm). 30 ppm =
                                 * ~0.05 cents, inaudible even on a sustained sine. WAS 500.0 = 0.87
                                 * cents, which railed audibly (the "rubbery clock" shimmer) whenever
                                 * the loop corrected before convergence. Once locked, every step is
                                 * this small; warm-start seeds the period already-locked. */
#define PLL_ACQ_PPM      500.0  /* ACQUIRE clamp: used only BEFORE lock, when the buffer is mid
                                 * fill/drain and not yet a stable pitch reference -- a big step is
                                 * inaudible-in-context here and pulls the master's full crystal offset
                                 * (reac2 ~+568 ppm) out in ~1 min instead of hours. Drops to
                                 * PLL_MAXPPM the moment a trigger's required correction fits within it. */
static double g_pll_fgain = PLL_FGAIN;   /* runtime override (--pll-fgain) */
static int    g_pll_pos_min = 0;         /* position-control source: 0=mean depth (default), 1=tightest port (--pll-pos-min) */

/* STEADY free-running output clock (the primary audio-quality fix, design §3 + §3b).
 * The output cadence is the audio: the stagebox recovers its word clock from the emit
 * inter-frame interval, so any per-tick rate nudge IS audible jitter ("rubbery on
 * silences"). On-rig the fast drain servo's per-tick bias leaked WDS input jitter into
 * the output cadence. With --steady the emit period FREE-RUNS at the base period and is
 * NOT modulated per-tick or per-occupancy-swing: short-term WDS bursts are absorbed as
 * occupancy swings (ring depth) and never reach the cadence. Occupancy is used ONLY as a
 * GLACIAL trim -- the PLL, integrated over PLL_WIN ticks (seconds), nudges the base period
 * sub-ppm to null the long-term rate error. The fast servo is held off the emit path (its
 * bias is forced to 0) so it cannot chatter the cadence. PLC still fires on a TRUE empty,
 * and the operator-commanded retarget walk (live-reconfig depth change) is untouched -- it
 * is a separate, intentional, bounded latency move, not steady-state occupancy chasing. */
static int g_steady = 1;             /* default ON in v2 (--no-steady restores v1's servo-paced cadence) */

/* PLL warm-start: a glacial lock takes minutes to converge, and a cold start spends those
 * minutes audibly converging. Once converged we persist the locked emit period (ns) PER PORT
 * to a small state file; on startup we seed the base period from it so the daemon starts
 * already-locked (instant clean), then slow-tracks for long-term drift. Per port because each
 * router's crystal offset differs (reac1 ~124867 ns, reac2 ~124929 ns for the same 96k master).
 * A missing/invalid file is tolerated -- the daemon cold-starts exactly as before.
 *
 * Format (line-oriented text, one record per output port, '#'-comments + blanks ignored):
 *   <out_iface> <emit_period_ns> <rate_label_hz> <unix_epoch_saved>
 * e.g.  lan1 124867 96000 1749150000
 * The out_iface is the key (a router handles a fixed set of stagebox-side ports); the daemon
 * restores only the periods for the ports it is configured with, and only when the saved
 * rate label matches the rate it detected at startup (a 44.1<->48<->96 k change invalidates it). */
static const char *g_lockfile = "/etc/reac-repacer.lock";
static int   g_warm_start = 1;       /* default ON (--no-warm-start disables persist + restore) */
static int    g_detect_ms = 60;             /* rate-change detection window ms (--detect-ms) */
static int    g_detect_confirm = 2;         /* consecutive windows to confirm a family change (--detect-confirm) */
static int    g_mute_ms = 250;              /* silence audio across a rate-change transition (--mute-ms) */
static volatile long long g_mute_until = 0; /* ns deadline: emit silent audio until this time */
#define LOCK_SAVE_WIN  240000        /* persist the converged lock ~every 30 s @ 8000 fps */

struct iface { int fd; int ifindex; struct sockaddr_ll tx; };

/* one REAC port: its own ifaces, ring, counters and pacing state. The shared
 * clock (period/deadline, below) drives every stream's emit on the same tick. */
struct stream {
	char in_name[IFNAMSIZ], out_name[IFNAMSIZ];
	struct iface IN, OUT;            /* IN = tunnel side, OUT = stagebox side */

	/* SPSC ring: rx thread produces, the shared pacer consumes */
	uint8_t (*slot_buf)[SLOT_SZ];
	uint16_t *slot_len;
	volatile unsigned r_head, r_tail;
	int occ_cap;                    /* runaway safety only */

	/* counters */
	unsigned long long n_rx, n_tx, n_drop_full, n_underrun, n_txerr, n_ret, n_reterr, n_ctrl, n_plc, n_skip, n_hold;
	unsigned long long g_in_frames; /* every accepted input frame, for rate detection */
	unsigned long long last_in;     /* for continuous rate detection */

	/* per-port pacing state (persists across ticks) */
	int target;
	long long occ_sum; int occ_cnt;
	int edit_win, edit_owe, edit_gap;
	int t_floor, t_ceil, occ_min_win, adapt_cnt, gh, sd;
	int started, prefill;           /* per-port activation: detect input, then prefill, then emit */
	double occ_ema;                 /* smoothed occupancy -- the drain servo's input */
	unsigned emit_ctr; int last_idx, have_last;
	uint8_t plc_buf[SLOT_SZ];

	pthread_t rx_th, ret_th;
	int rx_cpu;                     /* core the rx reader is pinned to (!= the RT pacer core) */
};

static struct stream streams[MAX_STREAMS];
static int n_streams;

static void on_sig(int s) { (void)s; running = 0; }
static void on_hup(int s) { (void)s; got_hup = 1; }   /* defer the work to the main loop */

static long long ns_now(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ---- wired-clock meter (--clock-source local) --------------------------------------
 * Count REAC frames on the wired OUT port and divide by elapsed time: the wire is
 * lossless and jitter-light, so period = elapsed/(frames-1) converges on the sender's
 * crystal with precision ~1/T (sub-ppm after seconds). Every REAC stream in the rig is
 * synchronous to ONE crystal (the boxes lock to the master), so the wired cadence IS
 * the exact rate the emit needs: rate exact -> occupancy holds -> the occupancy-chasing
 * loops (servo/tracker/PLL recovery) are bypassed entirely and never modulate the
 * cadence. WiFi carries data, never timing. */
static int g_clock_local, g_clock_in;            /* --clock-source local (OUT wired dev) / local-in (IN = mixer rate) */
static int g_clock_margin_ppm = 2;              /* --clock-margin-ppm: cumulative-rate change that re-applies the pace */
/* --inject-sine SLOT:FREQ -- TEST ONLY: overwrite one channel slot of every emitted
 * audio frame (port 0) with a locally synthesized sine. Isolates the delivery path:
 * the desk receives a tone that never touched the stagebox A/D or the WiFi. */
static int g_inj_slot = -1;
static double g_inj_freq = 440.0, g_inj_ph = 0.0, g_inj_amp = 1.0e6;   /* ~-18 dBFS */
/* --inject-copy SRC:DST -- TEST ONLY: relay the DOWNSTREAM's slot SRC samples (the
 * desk's own oscillator, arriving wired on the OUT port) into the upstream slot DST.
 * A digital loopback of the desk's pristine samples through our delivery path: no
 * synthesis, no A/D, no WiFi in the audio. */
static int g_cp_src = -1, g_cp_dst = -1;
static int g_pace_ds;   /* --pace-by-downstream: emit each upstream frame as a timed RESPONSE to a
                         * downstream frame arriving on the wired OUT port -- a real stagebox is a
                         * synchronous TDM slave (measured: box answers 69 us +/- 25 after each
                         * downstream frame; our free-running emit smeared phase across the whole
                         * period and the desk garbled it). The desk's own cable provides rate AND
                         * phase by construction; the ring is purely a WDS jitter absorber. */
#define CPR_SZ 16384
static int32_t g_cpr[CPR_SZ];
static volatile unsigned g_cpr_w, g_cpr_r;

static void *inj_copy_rx(void *arg) {
	struct stream *s = arg; uint8_t buf[SLOT_SZ];
	while (recv(s->OUT.fd, buf, sizeof buf, MSG_DONTWAIT) > 0) ;
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(s->OUT.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 50) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;
		uint8_t *r = buf; int len = (int)n;
		if (len > 21 && r[12] == 0x81 && r[13] == 0x00 && r[16] == 0x88 && r[17] == 0x19) { memmove(r + 12, r + 16, (size_t)(len - 16)); len -= 4; }
		if (!(r[12] == 0x88 && r[13] == 0x19) || !(r[16] == 0 && r[17] == 0)) continue;
		int nch = (len - 50) / 36; if (nch < 1 || g_cp_src >= nch) continue;
		uint8_t *a = r + 50; int base = (g_cp_src & ~1) * 3;
		for (int s2 = 0; s2 < 12; s2++) {
			uint8_t *sp = a + base + s2 * (nch * 3);
			uint8_t b0, b1, b2;
			if (g_cp_src & 1) { b0 = sp[4]; b1 = sp[5]; b2 = sp[2]; }
			else              { b0 = sp[3]; b1 = sp[0]; b2 = sp[1]; }
			int32_t v = b0 | (b1 << 8) | (b2 << 16); if (v & 0x800000) v -= (1 << 24);
			unsigned w = g_cpr_w;
			g_cpr[w & (CPR_SZ - 1)] = v;
			__atomic_store_n(&g_cpr_w, w + 1, __ATOMIC_RELEASE);
		}
	}
	return NULL;
}

static void inj_copy(uint8_t *f, int len) {
	if (!(f[16] == 0 && f[17] == 0) || len < 50) return;
	int nch = (len - 50) / 36; if (nch < 1 || g_cp_dst >= nch) return;
	uint8_t *a = f + 50; int base = (g_cp_dst & ~1) * 3;
	static int32_t last;
	static int primed;
	unsigned w0 = __atomic_load_n(&g_cpr_w, __ATOMIC_ACQUIRE);
	if (!primed) {                      /* let the reader get ~21 ms ahead before the first pop,
	                                     * so thread-scheduling jitter never empties the ring */
		if (w0 - g_cpr_r < 2048) return;
		g_cpr_r = w0 - 2048; primed = 1;
	}
	for (int s2 = 0; s2 < 12; s2++) {
		unsigned w = __atomic_load_n(&g_cpr_w, __ATOMIC_ACQUIRE);
		int32_t v = last;
		if (w != g_cpr_r) { v = g_cpr[g_cpr_r & (CPR_SZ - 1)]; g_cpr_r++; last = v; }
		uint8_t b0 = v & 0xff, b1 = (v >> 8) & 0xff, b2 = (v >> 16) & 0xff;
		uint8_t *sp = a + base + s2 * (nch * 3);
		if (g_cp_dst & 1) { sp[4] = b0; sp[5] = b1; sp[2] = b2; }
		else              { sp[3] = b0; sp[0] = b1; sp[1] = b2; }
	}
}

static void inj_sine(uint8_t *f, int len) {
	if (!(f[16] == 0 && f[17] == 0) || len < 50) return;          /* audio frames only */
	int nch = (len - 50) / 36; if (nch < 1) return;
	if (g_inj_slot >= nch) return;
	uint8_t *a = f + 50;
	int base = (g_inj_slot & ~1) * 3;
	for (int s2 = 0; s2 < 12; s2++) {
		int32_t v = (int32_t)(g_inj_amp * sin(g_inj_ph));
		g_inj_ph += 2.0 * M_PI * g_inj_freq / 96000.0;
		if (g_inj_ph > 2.0 * M_PI) g_inj_ph -= 2.0 * M_PI;
		uint8_t b0 = v & 0xff, b1 = (v >> 8) & 0xff, b2 = (v >> 16) & 0xff;
		uint8_t *sp = a + base + s2 * (nch * 3);
		if (g_inj_slot & 1) { sp[4] = b0; sp[5] = b1; sp[2] = b2; }
		else                { sp[3] = b0; sp[0] = b1; sp[1] = b2; }
	}
}
static volatile unsigned long long g_met_n;     /* driver-level rx_packets of the OUT iface */
static volatile long long g_met_tlast;          /* when that count was read */

static int open_iface(const char *name, struct iface *o) {
	int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (s < 0) { perror("socket"); return -1; }
	struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { perror(name); close(s); return -1; }
	struct sockaddr_ll sll; memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET; sll.sll_protocol = htons(ETH_P_ALL); sll.sll_ifindex = ifr.ifr_ifindex;
	if (bind(s, (struct sockaddr *)&sll, sizeof sll) < 0) { perror("bind"); close(s); return -1; }
	int rcv = 32 * 1024 * 1024;   /* burst headroom: 43 ms WDS bursts must not overflow the reader's socket */
	if (setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof rcv) < 0) setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
	if (g_bypass) { int one = 1; setsockopt(s, SOL_PACKET, PACKET_QDISC_BYPASS, &one, sizeof one); }
	if (g_etf) { struct sock_txtime st = { .clockid = CLOCK_TAI, .flags = 0 }; setsockopt(s, SOL_SOCKET, SO_TXTIME, &st, sizeof st); }
	o->fd = s; o->ifindex = ifr.ifr_ifindex;
	memset(&o->tx, 0, sizeof o->tx);
	o->tx.sll_family = AF_PACKET; o->tx.sll_ifindex = ifr.ifr_ifindex; o->tx.sll_halen = 6;
	return 0;
}

/* forward RX: tunnel master broadcast -> this stream's ring */
static void *fwd_rx(void *arg) {
	struct stream *s = arg; uint8_t buf[SLOT_SZ];
	if (s->rx_cpu >= 0) {   /* pin off the RT pacer core so the pacer can't preempt this reader mid-burst */
		cpu_set_t set; CPU_ZERO(&set); CPU_SET(s->rx_cpu, &set);
		sched_setaffinity(0, sizeof set, &set);
	}
	while (recv(s->IN.fd, buf, sizeof buf, MSG_DONTWAIT) > 0) ;   /* flush stale backlog -> no startup overshoot */
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(s->IN.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 18) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;          /* skip our own return TX */
		if (!(buf[12] == 0x88 && buf[13] == 0x19)) continue;        /* REAC ethertype */
		if (g_bcast_only && buf[0] != 0xff) continue;               /* master broadcast only */
		s->g_in_frames++;                                           /* input frame rate (rate detect) */
		/* ALL frame types ride ONE master counter sequence at a constant packet rate:
		 * the control frames (cdea channel-map, cfea announce) occupy counter slots
		 * interspersed with 0000 audio -- they replace audio slots rather than adding
		 * to the stream, so audio+control always sum to the packet rate. Each control
		 * frame also carries audio samples in its slot, so emitting it early
		 * (--ctrl-bypass) puts those samples ahead of cadence: one click per control
		 * frame. Default keeps every type IN SEQUENCE through the ring; in == out, so
		 * latency stays flat even through a control burst. */
		int is_ctrl = !(buf[16] == 0x00 && buf[17] == 0x00);
		if (is_ctrl) {
			s->n_ctrl++;
			if (g_ctrl_bypass) {   /* A/B only: reproduces the heartbeat artifact */
				sendto(s->OUT.fd, buf, (size_t)n, 0, (struct sockaddr *)&s->OUT.tx, sizeof s->OUT.tx);
				continue;
			}
		}
		unsigned tail = s->r_tail, next = (tail + 1) & RING_MASK;
		unsigned occ_now = (tail - __atomic_load_n(&s->r_head, __ATOMIC_ACQUIRE)) & RING_MASK;
		if (s->occ_cap && (int)occ_now >= s->occ_cap) { s->n_drop_full++; continue; }  /* bound latency */
		if (next == __atomic_load_n(&s->r_head, __ATOMIC_ACQUIRE)) { s->n_drop_full++; continue; }
		memcpy(s->slot_buf[tail], buf, (size_t)n); s->slot_len[tail] = (uint16_t)n;
		__atomic_store_n(&s->r_tail, next, __ATOMIC_RELEASE); s->n_rx++;
	}
	return NULL;
}

/* return relay: stagebox -> tunnel, immediate (master tolerates input jitter) */
static void *ret_relay(void *arg) {
	struct stream *s = arg; uint8_t buf[SLOT_SZ];
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(s->OUT.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 14) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;          /* never echo the master back -> no loop */
		if (!(buf[12] == 0x88 && buf[13] == 0x19)) continue;        /* REAC frames (incl handshake) */
		if (sendto(s->IN.fd, buf, (size_t)n, 0, (struct sockaddr *)&s->IN.tx, sizeof s->IN.tx) < 0) s->n_reterr++;
		else s->n_ret++;
	}
	return NULL;
}

static int g_meter_cpu = -1;   /* core for the meter -- OFF the pacer's RT core */
static void *clk_meter(void *arg) {
	/* Count the wired device's REAC FRAME COUNTER (bytes 14-15), drop-immune. CRITICAL:
	 * this must NOT read-while-the-pacer-loops on the same core (a busy socket read steals
	 * scheduling from the SCHED_FIFO emit -> cadence jitter). So: own socket with a TINY
	 * rcvbuf, pinned OFF the pacer core, drained to the NEWEST counter then SLEEP 250 ms.
	 * A few hundred reads/s off-core, not 16k/s on-core. Drops don't matter -- the counter
	 * advances regardless, so newest-counter / elapsed is the device's exact rate. */
	struct stream *s = arg; uint8_t buf[SLOT_SZ];
	if (g_meter_cpu >= 0) { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_meter_cpu, &cs); sched_setaffinity(0, sizeof cs, &cs); }
	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) return NULL;
	struct sockaddr_ll sll; memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET; sll.sll_protocol = htons(ETH_P_ALL); sll.sll_ifindex = g_clock_in ? s->IN.ifindex : s->OUT.ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) { close(fd); return NULL; }
	int rb = 128 * 1024;   /* tiny: we only need the newest counter, the rest can drop */
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
	unsigned long long cum = 0; uint16_t lastc = 0; int have = 0;
	while (running) {
		struct timespec ts = { 0, 250000000 };                    /* 250 ms between samples, off-core */
		nanosleep(&ts, NULL);
		struct sockaddr_ll from; socklen_t fl;
		for (;;) { fl = sizeof from; if (recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl) <= 0) break; }   /* discard stale backlog */
		int got = 0;                                              /* then BLOCK for one FRESH frame -> (counter,time) is current */
		while (running && !got) {
			fl = sizeof from;
			ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
			if (n <= 0) break;
			if (from.sll_pkttype == PACKET_OUTGOING) continue;
			int eo = (n > 13 && buf[12] == 0x81 && buf[13] == 0x00) ? 16 : 12;
			if (n < eo + 6 || !(buf[eo] == 0x88 && buf[eo + 1] == 0x19) || n < 1000) continue;
			uint16_t c = buf[eo + 2] | (buf[eo + 3] << 8);
			if (have) cum += (uint16_t)(c - lastc);
			lastc = c; have = 1; g_met_n = cum; g_met_tlast = ns_now();
			got = 1;
		}
	}
	close(fd); return NULL;
}
/* one emission with pace_one's exact semantics (counter restamp, mute, inject, PLC) */
static void ds_emit_one(struct stream *s) __attribute__((unused));
static void ds_emit_one(struct stream *s) {
	unsigned head = s->r_head, tail = __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE);
	int occ = (int)((tail - head) & RING_MASK);
	if (occ <= 0) {
		s->n_underrun++;
		if (s->have_last) {
			memcpy(s->plc_buf, s->slot_buf[s->last_idx], s->slot_len[s->last_idx]);
			s->plc_buf[14] = s->emit_ctr & 0xFF; s->plc_buf[15] = (s->emit_ctr >> 8) & 0xFF;
			if (g_mute_until && ns_now() < g_mute_until && s->plc_buf[16] == 0 && s->plc_buf[17] == 0 && s->slot_len[s->last_idx] > 18)
				memset(s->plc_buf + 18, 0, s->slot_len[s->last_idx] - 18);
			if (g_inj_slot >= 0 && s == &streams[0]) inj_sine(s->plc_buf, s->slot_len[s->last_idx]);
			if (g_cp_dst >= 0 && s == &streams[0]) inj_copy(s->plc_buf, s->slot_len[s->last_idx]);
			if (tx(s->OUT.fd, s->plc_buf, s->slot_len[s->last_idx], &s->OUT.tx) < 0) s->n_txerr++;
			else s->n_plc++;
			s->emit_ctr = (s->emit_ctr + 1) & 0xFFFF;
		}
		return;
	}
	uint8_t *f = s->slot_buf[head];
	if (!s->have_last) s->emit_ctr = f[14] | (f[15] << 8);
	f[14] = s->emit_ctr & 0xFF; f[15] = (s->emit_ctr >> 8) & 0xFF;
	if (g_mute_until && ns_now() < g_mute_until && f[16] == 0 && f[17] == 0 && s->slot_len[head] > 18)
		memset(f + 18, 0, s->slot_len[head] - 18);
	if (g_inj_slot >= 0 && s == &streams[0]) inj_sine(f, s->slot_len[head]);
	if (g_cp_dst >= 0 && s == &streams[0]) inj_copy(f, s->slot_len[head]);
	if (tx(s->OUT.fd, f, s->slot_len[head], &s->OUT.tx) < 0) s->n_txerr++;
	s->last_idx = (int)head; s->have_last = 1;
	__atomic_store_n(&s->r_head, (head + 1) & RING_MASK, __ATOMIC_RELEASE); s->n_tx++;
	s->emit_ctr = (s->emit_ctr + 1) & 0xFFFF;
}

static int g_ds_cpu = -1;            /* pin the slot-grid listener to the RT core (set from --cpu) */
static volatile long long g_ds_tns;  /* arrival time of the latest downstream frame (the desk's slot grid) */
/* slot-grid listener: pure trigger-emission was tried and REJECTED on the rig -- it
 * phase-locks but adds ~38 us of per-frame cadence jitter (recvfrom scheduling), and
 * the desk needs BOTH smooth cadence and slot phase. So this thread only TIMESTAMPS
 * the desk's downstream frames; the timed pacer keeps its glacial cadence and steers
 * its deadline PHASE slowly (ns per tick, heavily filtered) onto the grid. */
static void *ds_pacer(void *arg) {
	struct stream *s = arg; uint8_t buf[SLOT_SZ];
	struct sched_param prm; prm.sched_priority = 79;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &prm);
	if (g_ds_cpu >= 0) { cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_ds_cpu, &cs); sched_setaffinity(0, sizeof cs, &cs); }
	while (recv(s->OUT.fd, buf, sizeof buf, MSG_DONTWAIT) > 0) ;
	while (running) {
		struct sockaddr_ll from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(s->OUT.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 18) continue;
		if (from.sll_pkttype == PACKET_OUTGOING) continue;
		if (!((buf[12] == 0x88 && buf[13] == 0x19) ||
		      (buf[12] == 0x81 && buf[13] == 0x00 && buf[16] == 0x88 && buf[17] == 0x19))) continue;
		g_ds_tns = ns_now();
	}
	return NULL;
}

static long nearest_std_pps(double pps) {
	static const long t[] = { 3675, 4000, 8000 };   /* 44.1 / 48 / 96 kHz */
	long best = t[0]; double bd = 1e18;
	for (unsigned i = 0; i < 3; i++) { double d = pps - (double)t[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = t[i]; } }
	return best;
}

/* sample rate (Hz) a frame rate (pps) belongs to: 12 samples per REAC frame */
static long rate_label_hz(long period_ns) {
	if (period_ns < 1) return 0;
	double pps = 1.0e9 / (double)period_ns;
	return nearest_std_pps(pps) * 12;
}

/* ---- PLL warm-start: persist + restore the converged per-port emit period ---------
 *
 * lock_load: look up the saved emit period (ns) for out_name, returning it (or 0 if
 *   absent/invalid) so the caller can seed the PLL already-locked. Only accepts a record
 *   whose saved rate label matches rate_hz (a 44.1/48/96 k change since the save voids it)
 *   and whose period is within a sane +/-5% of the standard for that rate. */
static long lock_load(const char *out_name, long rate_hz) {
	if (!g_warm_start) return 0;
	FILE *f = fopen(g_lockfile, "r");
	if (!f) return 0;
	char line[160]; long found = 0;
	while (fgets(line, sizeof line, f)) {
		char *p = line; while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0') continue;
		char nm[IFNAMSIZ]; long per = 0, lab = 0; long long ts = 0;
		if (sscanf(p, "%15s %ld %ld %lld", nm, &per, &lab, &ts) < 3) continue;
		if (strcmp(nm, out_name)) continue;
		if (lab != rate_hz) continue;                                  /* rate changed -> stale */
		long std_per = (long)(1.0e9 / (double)(nearest_std_pps(1.0e9 / (double)per)) + 0.5);
		if (per < std_per - std_per / 20 || per > std_per + std_per / 20) continue;  /* +/-5% sanity */
		found = per;            /* last matching record wins (newest appended) */
	}
	fclose(f);
	return found;
}

/* lock_save: rewrite the state file with the live converged period for every active port,
 * preserving entries for ports this daemon does NOT handle (a different router/instance may
 * own them). Written to a temp file + rename so a crash never leaves a half-written lock.
 * period_ns is the daemon's single shared base period; each active port's crystal offset is
 * already folded into it (one daemon == one router == one recovered clock), so every active
 * port of this daemon records the same value -- the per-port keying lets a DIFFERENT router's
 * daemon restore its own ports' (differently-offset) periods from the same shared file. */
static void lock_save(long period_ns) {
	if (!g_warm_start) return;
	long rate_hz = rate_label_hz(period_ns);
	char tmp[256]; snprintf(tmp, sizeof tmp, "%s.tmp", g_lockfile);
	FILE *out = fopen(tmp, "w");
	if (!out) return;
	fprintf(out, "# reac-repacer PLL warm-start lock -- per-port converged emit period.\n");
	fprintf(out, "# <out_iface> <emit_period_ns> <rate_label_hz> <unix_epoch_saved>\n");
	long long now = (long long)time(NULL);
	/* carry over records for ports we do not own (untouched lines), skipping ones we rewrite */
	FILE *in = fopen(g_lockfile, "r");
	if (in) {
		char line[160];
		while (fgets(line, sizeof line, in)) {
			char *p = line; while (*p == ' ' || *p == '\t') p++;
			if (*p == '#' || *p == '\n' || *p == '\0') continue;
			char nm[IFNAMSIZ];
			if (sscanf(p, "%15s", nm) != 1) continue;
			int ours = 0;
			for (int i = 0; i < n_streams; i++) if (!strcmp(streams[i].out_name, nm)) { ours = 1; break; }
			if (ours) continue;                       /* will be re-emitted fresh below */
			fputs(line, out);
		}
		fclose(in);
	}
	for (int i = 0; i < n_streams; i++) {
		if (streams[i].n_rx == 0 || !streams[i].started) continue;   /* only ports actually locked */
		fprintf(out, "%s %ld %ld %lld\n", streams[i].out_name, period_ns, rate_hz, now);
	}
	fclose(out);
	rename(tmp, g_lockfile);                           /* atomic publish */
}

/* recompute the per-port window sizes that depend on the slot period */
static void stream_resize(struct stream *s, long period_ns, int prefill) {
	s->edit_win = (int)(1.0e9 / (double)period_ns); if (s->edit_win < 1000) s->edit_win = 1000;
	s->t_floor = (int)((long long)g_adapt_min_ms * 1000000 / period_ns); if (s->t_floor < 4) s->t_floor = 4;
	s->t_ceil  = (int)((long long)g_adapt_max_ms * 1000000 / period_ns); if (s->t_ceil > RING_SZ - 64) s->t_ceil = RING_SZ - 64;
	s->target = prefill;
	s->prefill = prefill;
	s->occ_ema = (double)prefill;
	s->started = 0;          /* re-arm per-port activation (prefill before emitting) */
	if (g_clock_local) {     /* local mode: idle at prefill = the max-latency budget; absorb bursts up to 2x */
		s->occ_cap = prefill * 2;
		if (s->occ_cap > RING_SZ - 64) s->occ_cap = RING_SZ - 64;
		if (s->occ_cap < 8) s->occ_cap = 8;
	}
}

/* one port's emit for this tick. The frozen shared clock owns the cadence; here we
 * spread one scheduled frame-edit per tick (drop = latency down, hold = latency up),
 * conceal underruns, and size this port's buffer to its own burst depth. Returns the
 * port's current occupancy; sets *active when the port has real data flowing. */
static int pace_one(struct stream *s, int *active, int eq_floor, int eq_done, int join_depth) {
	/* a port that has never received a frame stays dormant: no emit, no underrun
	 * count, no buffer growth. It wakes up cleanly the instant input appears (e.g.
	 * a stagebox patched in after the daemon is already running). */
	if (s->n_rx == 0) { *active = 0; return 0; }
	unsigned head = s->r_head, tail = __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE);
	int occ = (int)((tail - head) & RING_MASK);
	/* UNISON occupancy-anchored activation: gate the FIRST emit on the COMMON floor, not this
	 * port's own prefill, so every port begins at the same depth = the same output delay. A
	 * shallow port DEFERS (keeps filling, does NOT emit) until it reaches the floor -- this ADDS
	 * buffer, never drops a frame, so it is click-free at lock. Pre eq_done the floor is the
	 * snapshotted common floor (the barrier); post eq_done a late/idle hot-joiner gates on
	 * join_depth = the peers' live running depth, so it joins delay-matched. ONE-TIME: once
	 * s->started it is never re-armed (only a rate-change relock re-arms). */
	if (!s->started) {
		int floor = eq_done ? join_depth : eq_floor;
		if (floor < 1) floor = 1;
		if (floor > s->occ_cap - 1) floor = s->occ_cap - 1;   /* deadlock guard: rx drops at occ_cap, so the gate must be reachable */
		if (occ < floor) { *active = 0; return occ; }          /* DEFER: keep filling, do not emit yet */
		s->started = 1;
		s->target = floor;
		s->occ_ema = (double)floor;
		s->occ_min_win = 1 << 30; s->adapt_cnt = 0; s->gh = 0; s->sd = 0;
		fprintf(stderr, "port %s->%s: REAC detected, filled to common depth %d -> active\n", s->in_name, s->out_name, floor);
	}
	/* Latency is controlled ONLY by the shared clock rate (the drain servo in the pacing
	 * loop), never by dropping or holding frames -- so a clean signal is never clicked.
	 * Here we just emit the next frame in sequence, or conceal a genuine underrun by
	 * repeating the last frame under the next counter (a verbatim repeat would reuse a
	 * counter and read as 65535 lost frames at the slave). */
	if (occ <= 0) {
		s->n_underrun++;
		if (s->have_last) {
			memcpy(s->plc_buf, s->slot_buf[s->last_idx], s->slot_len[s->last_idx]);
			s->plc_buf[14] = s->emit_ctr & 0xFF; s->plc_buf[15] = (s->emit_ctr >> 8) & 0xFF;
			if (g_mute_until && ns_now() < g_mute_until && s->plc_buf[16] == 0 && s->plc_buf[17] == 0 && s->slot_len[s->last_idx] > 18)
				memset(s->plc_buf + 18, 0, s->slot_len[s->last_idx] - 18);
			if (g_inj_slot >= 0 && s == &streams[0]) inj_sine(s->plc_buf, s->slot_len[s->last_idx]);
			if (g_cp_dst >= 0 && s == &streams[0]) inj_copy(s->plc_buf, s->slot_len[s->last_idx]);
			if (tx(s->OUT.fd, s->plc_buf, s->slot_len[s->last_idx], &s->OUT.tx) < 0) s->n_txerr++;
			else s->n_plc++;
			s->emit_ctr = (s->emit_ctr + 1) & 0xFFFF;
		}
	} else {
		uint8_t *f = s->slot_buf[head];
		if (!s->have_last) s->emit_ctr = f[14] | (f[15] << 8);                /* seed the counter */
		f[14] = s->emit_ctr & 0xFF; f[15] = (s->emit_ctr >> 8) & 0xFF;        /* own a monotonic output counter */
		if (g_mute_until && ns_now() < g_mute_until && f[16] == 0 && f[17] == 0 && s->slot_len[head] > 18)
			memset(f + 18, 0, s->slot_len[head] - 18);   /* transition mute: silence audio payload, keep link control */
		if (g_inj_slot >= 0 && s == &streams[0]) inj_sine(f, s->slot_len[head]);
		if (g_cp_dst >= 0 && s == &streams[0]) inj_copy(f, s->slot_len[head]);
		if (tx(s->OUT.fd, f, s->slot_len[head], &s->OUT.tx) < 0) s->n_txerr++;
		s->last_idx = (int)head; s->have_last = 1;
		__atomic_store_n(&s->r_head, (head + 1) & RING_MASK, __ATOMIC_RELEASE); s->n_tx++;
		s->emit_ctr = (s->emit_ctr + 1) & 0xFFFF;
	}
	/* smoothed occupancy (~0.5 s) + per-port low-water mark. These feed the ONE shared
	 * target + drain servo in the pacing loop: all ports share a clock and a source, so a
	 * single target sized to the worst dip across ports keeps them converged (per-port
	 * targets would diverge once an underrun offsets one port's occupancy from the rest). */
	s->occ_ema += ((double)occ - s->occ_ema) * (1.0 / 2048.0);
	if (g_adapt && occ < s->occ_min_win) s->occ_min_win = occ;
	*active = s->started;
	return occ;
}

/* reset a port's pacing state and drop its ring (used on a live rate change) */
static void stream_relock(struct stream *s, long period_ns, int prefill) {
	stream_resize(s, period_ns, prefill);
	s->occ_sum = 0; s->occ_cnt = 0; s->edit_owe = 0; s->edit_gap = 0;
	s->occ_min_win = 1 << 30; s->adapt_cnt = 0; s->gh = 0; s->sd = 0;
	__atomic_store_n(&s->r_head, __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

/* sum input frames across all ports (rate detect uses the busiest port's rate; all
 * ports of one mixer share a rate, so any active port gives the answer) */
static double measure_pps(unsigned long long *base, double secs) {
	double best = 0;
	for (int i = 0; i < n_streams; i++) {
		double p = secs > 0 ? (double)(streams[i].g_in_frames - base[i]) / secs : 0;
		if (p > best) best = p;
	}
	return best;
}

static int add_stream(const char *in, const char *out) {
	if (n_streams >= MAX_STREAMS) { fprintf(stderr, "too many ports (max %d)\n", MAX_STREAMS); return -1; }
	struct stream *s = &streams[n_streams];
	strncpy(s->in_name, in, IFNAMSIZ - 1);
	strncpy(s->out_name, out, IFNAMSIZ - 1);
	n_streams++;
	return 0;
}

/* ---- live reconfiguration (no-restart retune, no dropout) -------------------------
 *
 * The pacing loop owns its tuning state on its stack. pacer_live points at the pieces a
 * live retune touches so the ubus set / SIGHUP path can adjust them from inside the loop
 * (both triggers are serviced ON the pacing thread, so there is no cross-thread race --
 * the ubus socket is drained by ubus_handle_event from the loop, and SIGHUP only sets a
 * flag the loop reads). prefill_ms is the live target depth in ms; the loop keeps the
 * frame-count target (prefill / shtgt) derived from it and the current period. */
struct pacer_live {
	int    prefill_ms;            /* live target depth (ms) -- the source of the occupancy target */
	long   period_ns;            /* current base period (the frozen recovered clock) */
	int   *prefill;              /* loop's target occupancy (frames), kept == prefill_ms @ period */
	double *shtgt;               /* shared adaptive target (frames) the servo/PLL steer toward */
	int   *t_floor, *t_ceil;     /* adapt clamps (frames) derived from adapt_min/max_ms */
	double *max_ppm;             /* servo clamp (ppm) derived from g_servo_clamp_ppm */
	double *retarget_ppm;        /* extra clock bias held during a depth ramp (+ = drain/shrink) */
	int    *retarget_ticks;      /* ticks remaining in the current depth ramp */
};

/* clamp the staged hot params to the same ranges the CLI/UCI accept, in place */
static void hot_params_clamp(struct hot_params *p) {
	if (p->prefill_ms < 1)    p->prefill_ms = 1;
	if (p->prefill_ms > 1000) p->prefill_ms = 1000;       /* bounded by the ring (clamped again to occ_cap below) */
	if (p->adapt_min_ms < 1)  p->adapt_min_ms = 1;
	if (p->adapt_max_ms < p->adapt_min_ms) p->adapt_max_ms = p->adapt_min_ms;
	if (p->adapt_max_ms > 1000) p->adapt_max_ms = 1000;
	if (p->adapt_margin < 0)  p->adapt_margin = 0;
	if (p->adapt_margin > RING_SZ - 64) p->adapt_margin = RING_SZ - 64;
	if (p->servo_clamp_ppm < 0) p->servo_clamp_ppm = 0;
	if (p->servo_clamp_ppm > 50000) p->servo_clamp_ppm = 50000;
}

/* The glitch-free occupancy retarget + hot-param apply. Called from the pacing loop when
 * a retune was staged. Updates the global hot params (g_adapt_*, g_servo_clamp_ppm) and
 * the loop's derived state, and -- on a target-depth change -- schedules a gentle ramp
 * that walks the LIVE occupancy to the new target by biasing the clock for a short while.
 * NO frame is dropped and the ring is NOT flushed: audio stays continuous and only the
 * latency walks to the new value. Grow = hold output back (negative bias = slower clock),
 * shrink = run output ahead (positive bias = faster clock).
 *
 * Physics: moving the latency by N frames means the output emits N more/fewer frames than
 * it consumes over the walk, i.e. a rate offset of N/walk_frames. A small by-ear nudge (a
 * few ms) glides in the nominal RETARGET_MS at a sub-cent, inaudible rate; a large jump is
 * held at RETARGET_MAXPPM (a transient sub-4-cent glide -- well inside the band the servo
 * already treats as inaudible) and the walk simply takes longer, since latency cannot be
 * shifted by X ms in much less than X ms of wall time without an audible pitch step. */
#define RETARGET_MS     400.0    /* nominal walk time for a small depth nudge */
#define RETARGET_MAXPPM 2000.0   /* bias ceiling for the walk (~3.5 cents, transient): a
                                  * larger jump stretches the duration rather than exceed it,
                                  * keeping the glide inaudible */
static void apply_hot_params(struct pacer_live *L, const struct hot_params *in) {
	struct hot_params p = *in;
	hot_params_clamp(&p);

	/* the non-depth hot params take effect immediately via their globals + derived locals */
	g_adapt_min_ms    = p.adapt_min_ms;
	g_adapt_max_ms    = p.adapt_max_ms;
	g_adapt_margin    = p.adapt_margin;
	g_servo_clamp_ppm = p.servo_clamp_ppm;
	*L->t_floor = (int)((long long)g_adapt_min_ms * 1000000 / L->period_ns); if (*L->t_floor < 4) *L->t_floor = 4;
	*L->t_ceil  = (int)((long long)g_adapt_max_ms * 1000000 / L->period_ns); if (*L->t_ceil > RING_SZ - 64) *L->t_ceil = RING_SZ - 64;
	*L->max_ppm = (double)g_servo_clamp_ppm;

	/* target depth: convert ms -> frames at the live period, clamp into the ring */
	int new_pf = (int)((long long)p.prefill_ms * 1000000 / L->period_ns);
	if (new_pf < 1) new_pf = 1;
	if (new_pf > RING_SZ - 65) new_pf = RING_SZ - 65;
	if (n_streams > 0 && new_pf > streams[0].occ_cap - 1) new_pf = streams[0].occ_cap - 1;
	int old_pf = *L->prefill;
	L->prefill_ms = p.prefill_ms;
	*L->prefill = new_pf;
	*L->shtgt   = (double)new_pf;          /* the servo/PLL now steer toward the new depth */
	for (int i = 0; i < n_streams; i++) streams[i].target = new_pf;

	/* schedule the glitch-free walk only if the depth actually moved. Each biased tick moves
	 * occupancy by ppm*1e-6 slots (output runs ahead at +ppm = drain), so walking |delta|
	 * slots over N ticks needs ppm = -delta*1e6/N (grow=hold back=neg, shrink=ahead=pos). Aim
	 * for a RETARGET_MS walk; if that would exceed RETARGET_MAXPPM, hold the bias at the cap
	 * and stretch N so each step stays inaudible. */
	int delta = new_pf - old_pf;           /* >0 grow (more latency), <0 shrink */
	if (delta != 0) {
		double fps = 1.0e9 / (double)L->period_ns;
		double ticks = RETARGET_MS * 1.0e-3 * fps; if (ticks < 1.0) ticks = 1.0;
		double ppm = -(double)delta * 1.0e6 / ticks;   /* -delta: grow=hold back(neg), shrink=ahead(pos) */
		if (ppm > RETARGET_MAXPPM || ppm < -RETARGET_MAXPPM) {
			double cap = RETARGET_MAXPPM;
			ticks = (double)(delta < 0 ? -delta : delta) * 1.0e6 / cap;   /* stretch to honour the cap */
			ppm = (delta > 0) ? -cap : cap;
		}
		*L->retarget_ppm   = ppm;
		*L->retarget_ticks = (int)(ticks + 0.5);
	} else {
		*L->retarget_ppm = 0.0; *L->retarget_ticks = 0;
	}
	fprintf(stderr, "reconfig: prefill=%dms(%d->%d slots) adapt=%d-%dms margin=%d servo-clamp=%dppm%s\n",
	        L->prefill_ms, old_pf, new_pf, g_adapt_min_ms, g_adapt_max_ms, g_adapt_margin, g_servo_clamp_ppm,
	        delta ? " [occupancy walk armed]" : "");
}

/* SIGHUP path: re-read the HOT params from /etc/config/reac-repacer via the uci CLI
 * (always present under procd; avoids linking libuci) and stage them. Anything absent
 * keeps the running value (passed in via cur). Done from the main loop, never the
 * handler. */
static int uci_get_int(const char *opt, int cur) {
	char cmd[160]; snprintf(cmd, sizeof cmd, "uci -q get reac-repacer.main.%s", opt);
	FILE *f = popen(cmd, "r");
	if (!f) return cur;
	char buf[64]; int v = cur;
	if (fgets(buf, sizeof buf, f)) { char *e; long n = strtol(buf, &e, 10); if (e != buf) v = (int)n; }
	pclose(f);
	return v;
}
static void reload_hot_params_from_uci(struct pacer_live *L) {
	struct hot_params p = {
		.prefill_ms      = uci_get_int("prefill_ms",      L->prefill_ms),
		.adapt_min_ms    = uci_get_int("adapt_min_ms",    g_adapt_min_ms),
		.adapt_max_ms    = uci_get_int("adapt_max_ms",    g_adapt_max_ms),
		.adapt_margin    = uci_get_int("adapt_margin",    g_adapt_margin),
		.servo_clamp_ppm = uci_get_int("servo_clamp_ppm", g_servo_clamp_ppm),
	};
	apply_hot_params(L, &p);
}

#ifdef HAVE_UBUS
/* ubus object "reac_repacer": get (live stats snapshot) + set (stage a hot retune). Both
 * callbacks run on the pacing thread (the loop drains the ubus socket itself), so set can
 * stage straight into g_pending and bump reconf_gen for the loop to apply on the next tick. */
static struct ubus_context *g_ubus_ctx;
static struct blob_buf g_ubus_b;
static struct pacer_live *g_live;         /* the loop publishes its address for get's snapshot */

enum { SET_PREFILL_MS, SET_ADAPT_MIN_MS, SET_ADAPT_MAX_MS, SET_ADAPT_MARGIN, SET_SERVO_CLAMP_PPM, __SET_MAX };
static const struct blobmsg_policy set_policy[__SET_MAX] = {
	[SET_PREFILL_MS]      = { .name = "prefill_ms",      .type = BLOBMSG_TYPE_INT32 },
	[SET_ADAPT_MIN_MS]    = { .name = "adapt_min_ms",    .type = BLOBMSG_TYPE_INT32 },
	[SET_ADAPT_MAX_MS]    = { .name = "adapt_max_ms",    .type = BLOBMSG_TYPE_INT32 },
	[SET_ADAPT_MARGIN]    = { .name = "adapt_margin",    .type = BLOBMSG_TYPE_INT32 },
	[SET_SERVO_CLAMP_PPM] = { .name = "servo_clamp_ppm", .type = BLOBMSG_TYPE_INT32 },
	/* a port/interface key here is structural -> rejected (see set_cb) */
};

static int repacer_get_cb(struct ubus_context *ctx, struct ubus_object *obj,
                          struct ubus_request_data *req, const char *method,
                          struct blob_attr *msg) {
	(void)obj; (void)method; (void)msg;
	blob_buf_init(&g_ubus_b, 0);
	blobmsg_add_u8(&g_ubus_b, "running", 1);
	if (g_live) {
		blobmsg_add_u32(&g_ubus_b, "prefill_ms", (uint32_t)g_live->prefill_ms);
		blobmsg_add_u32(&g_ubus_b, "target_slots", (uint32_t)*g_live->prefill);
		blobmsg_add_u32(&g_ubus_b, "period_ns", (uint32_t)g_live->period_ns);
		blobmsg_add_u32(&g_ubus_b, "retarget_active", (uint32_t)(*g_live->retarget_ticks > 0));
	}
	blobmsg_add_u32(&g_ubus_b, "adapt_min_ms", (uint32_t)g_adapt_min_ms);
	blobmsg_add_u32(&g_ubus_b, "adapt_max_ms", (uint32_t)g_adapt_max_ms);
	blobmsg_add_u32(&g_ubus_b, "adapt_margin", (uint32_t)g_adapt_margin);
	blobmsg_add_u32(&g_ubus_b, "servo_clamp_ppm", (uint32_t)g_servo_clamp_ppm);
	blobmsg_add_u8(&g_ubus_b, "adapt", g_adapt ? 1 : 0);
	blobmsg_add_u8(&g_ubus_b, "pll", g_pll ? 1 : 0);
	blobmsg_add_u32(&g_ubus_b, "ports", (uint32_t)n_streams);
	long pn = g_live ? g_live->period_ns : 1;   /* g_live is set before the object is registered */
	void *a = blobmsg_open_array(&g_ubus_b, "port");
	for (int i = 0; i < n_streams; i++) {
		struct stream *s = &streams[i];
		unsigned head = s->r_head, tail = __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE);
		int occ = (int)((tail - head) & RING_MASK);
		void *t = blobmsg_open_table(&g_ubus_b, NULL);
		blobmsg_add_string(&g_ubus_b, "in", s->in_name);
		blobmsg_add_string(&g_ubus_b, "out", s->out_name);
		blobmsg_add_u8(&g_ubus_b, "started", s->started ? 1 : 0);
		blobmsg_add_u32(&g_ubus_b, "occ", (uint32_t)occ);
		blobmsg_add_u32(&g_ubus_b, "occ_ms", (uint32_t)((long long)occ * pn / 1000000));
		blobmsg_add_u64(&g_ubus_b, "rx", s->n_rx);
		blobmsg_add_u64(&g_ubus_b, "tx", s->n_tx);
		blobmsg_add_u64(&g_ubus_b, "underrun", s->n_underrun);
		blobmsg_add_u64(&g_ubus_b, "plc", s->n_plc);
		blobmsg_close_table(&g_ubus_b, t);
	}
	blobmsg_close_array(&g_ubus_b, a);
	ubus_send_reply(ctx, req, g_ubus_b.head);
	return 0;
}

static int repacer_set_cb(struct ubus_context *ctx, struct ubus_object *obj,
                          struct ubus_request_data *req, const char *method,
                          struct blob_attr *msg) {
	(void)ctx; (void)obj; (void)method;
	/* reject structural keys outright: changing the port/interface mapping needs socket
	 * re-setup and a re-lock, which cannot be done live without a dropout. */
	struct blob_attr *cur; size_t rem;
	blobmsg_for_each_attr(cur, msg, rem) {
		const char *k = blobmsg_name(cur);
		if (!strcmp(k, "port") || !strcmp(k, "in_iface") || !strcmp(k, "out_iface") ||
		    !strcmp(k, "in") || !strcmp(k, "out") || !strcmp(k, "cpu"))
			return UBUS_STATUS_NOT_SUPPORTED;   /* structural / restart-only */
	}
	struct blob_attr *tb[__SET_MAX];
	blobmsg_parse(set_policy, __SET_MAX, tb, blob_data(msg), blob_len(msg));

	/* seed from the running values so an omitted key is left unchanged */
	struct hot_params p = {
		.prefill_ms      = g_live ? g_live->prefill_ms : 0,
		.adapt_min_ms    = g_adapt_min_ms,
		.adapt_max_ms    = g_adapt_max_ms,
		.adapt_margin    = g_adapt_margin,
		.servo_clamp_ppm = g_servo_clamp_ppm,
	};
	if (tb[SET_PREFILL_MS])      p.prefill_ms      = blobmsg_get_u32(tb[SET_PREFILL_MS]);
	if (tb[SET_ADAPT_MIN_MS])    p.adapt_min_ms    = blobmsg_get_u32(tb[SET_ADAPT_MIN_MS]);
	if (tb[SET_ADAPT_MAX_MS])    p.adapt_max_ms    = blobmsg_get_u32(tb[SET_ADAPT_MAX_MS]);
	if (tb[SET_ADAPT_MARGIN])    p.adapt_margin    = blobmsg_get_u32(tb[SET_ADAPT_MARGIN]);
	if (tb[SET_SERVO_CLAMP_PPM]) p.servo_clamp_ppm = blobmsg_get_u32(tb[SET_SERVO_CLAMP_PPM]);

	hot_params_clamp(&p);          /* validate ranges before staging */
	g_pending = p;
	reconf_gen++;                  /* the loop applies it on the next tick */

	/* reflect the (clamped) values that will be applied */
	blob_buf_init(&g_ubus_b, 0);
	blobmsg_add_u32(&g_ubus_b, "prefill_ms", (uint32_t)p.prefill_ms);
	blobmsg_add_u32(&g_ubus_b, "adapt_min_ms", (uint32_t)p.adapt_min_ms);
	blobmsg_add_u32(&g_ubus_b, "adapt_max_ms", (uint32_t)p.adapt_max_ms);
	blobmsg_add_u32(&g_ubus_b, "adapt_margin", (uint32_t)p.adapt_margin);
	blobmsg_add_u32(&g_ubus_b, "servo_clamp_ppm", (uint32_t)p.servo_clamp_ppm);
	ubus_send_reply(ctx, req, g_ubus_b.head);
	return 0;
}

static struct ubus_method repacer_methods[] = {
	UBUS_METHOD_NOARG("get", repacer_get_cb),
	UBUS_METHOD("set", repacer_set_cb, set_policy),
};
static struct ubus_object_type repacer_obj_type =
	UBUS_OBJECT_TYPE("reac_repacer", repacer_methods);
static struct ubus_object repacer_obj = {
	.name = "reac_repacer",
	.type = &repacer_obj_type,
	.methods = repacer_methods,
	.n_methods = ARRAY_SIZE(repacer_methods),
};

/* connect + register. Returns the socket fd to poll from the loop, or -1 if ubus is
 * unavailable (the daemon then runs without the live ubus control surface). */
static int ubus_setup(struct pacer_live *L) {
	g_live = L;
	g_ubus_ctx = ubus_connect(NULL);
	if (!g_ubus_ctx) { fprintf(stderr, "ubus: connect failed; live ubus control disabled\n"); return -1; }
	if (ubus_add_object(g_ubus_ctx, &repacer_obj) != 0) {
		fprintf(stderr, "ubus: add_object failed; live ubus control disabled\n");
		ubus_free(g_ubus_ctx); g_ubus_ctx = NULL; return -1;
	}
	return g_ubus_ctx->sock.fd;
}
/* drain any pending ubus request without blocking (called from the pacing loop) */
static void ubus_service(int fd) {
	if (!g_ubus_ctx) return;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) ubus_handle_event(g_ubus_ctx);
}
#else
/* no libubus: live control is SIGHUP-only; the get/set surface is absent */
static int  ubus_setup(struct pacer_live *L) { (void)L; return -1; }
static void ubus_service(int fd) { (void)fd; }
#endif

/* Print the full usage to the given stream. Listed defaults track the globals + the
 * main() locals below; every flag the parse loop understands appears here. */
static void usage(FILE *f) {
	fprintf(f,
"reac_repacer -- de-jitter / re-pacing relay for a Roland REAC stream over Wi-Fi/WDS.\n"
"\n"
"Buffers the bursty master broadcast and re-emits a constant cadence on a steady,\n"
"free-running clock to the local stagebox, so a clock-slave stagebox stays locked\n"
"across a jittery wireless path. One daemon handles one or more REAC ports.\n"
"\n"
"Usage: reac_repacer --port IN:OUT [--port IN:OUT ...] [options]\n"
"\n"
"At least one --port is required (there is no implicit default).\n"
"\n"
"Ports:\n"
"  --port IN:OUT          REAC port as an IN:OUT interface pair; repeat for each\n"
"                         port (IN = tunnel side, OUT = stagebox side). Required.\n"
"\n"
"Pacing / clock:\n"
"  --prefill-ms N         target buffer depth in ms before emitting (default 12)\n"
"  --servo-clamp-ppm N    drain-servo clock-bias clamp in ppm (default 700)\n"
"  --pll                  glacial frequency-lock: converge the base period to null\n"
"                         the long-term occupancy drift (default off)\n"
"  --no-steady            disable the steady free-running output clock and restore\n"
"                         the per-tick servo-paced cadence (steady is on by default)\n"
"\n"
"Adaptive window:\n"
"  --adapt                auto-size the buffer to the observed burst depth (default off)\n"
"  --adapt-min-ms N       floor latency in ms when shrinking (default 6)\n"
"  --adapt-max-ms N       ceiling latency in ms when growing (default 120)\n"
"  --adapt-margin N       keep the occupancy low-water mark this many slots above 0\n"
"                         (default 20)\n"
"  --reclaim              actively shrink latency (may drop frames -> clicks); the\n"
"                         default is grow-only (default off)\n"
"\n"
"Topology / placement:\n"
"  --forward-only         de-jitter only the forward (IN->OUT) direction and do not\n"
"                         run the return pass-through (mixer side; default off)\n"
"  --bcast-only           accept only the master broadcast frames (default off)\n"
"  --clock-source S       emit clock: wifi (occ PLL, default), local (OUT wired dev rate), local-in (IN/mixer rate)\n"
"                         (count frames on the wired OUT port = the master crystal,\n"
"                         exact; WiFi then carries data, never timing)\n"
"  --pace-by-downstream   emit each upstream frame as a response to a downstream frame\n"
"                         arriving on the wired OUT port (synchronous TDM, like a real\n"
"                         stagebox: the desk provides rate AND phase)\n"
"  --clock-margin-ms N    local mode: buffer movement (ms) that triggers a clock\n"
"                         re-derive from the cumulative count (default 3)\n"
"  --cpu N                core to pin the real-time pacing thread to (default 3)\n"
"\n"
"Warm-start state:\n"
"  --no-warm-start        do not persist or restore the converged lock (warm-start\n"
"                         is on by default)\n"
"  --lockfile PATH        warm-start state file (default /etc/reac-repacer.lock)\n"
"\n"
"  -h, --help             show this help and exit\n"
"\n"
"Examples:\n"
"  reac_repacer --port reactap.11:lan1 --port reactap.12:lan2 --prefill-ms 16\n"
"  reac_repacer --port reactap.11:lan1 --forward-only --adapt\n");
}

int main(int argc, char **argv) {
	/* --help/-h is handled before ANYTHING else (no rate detect, no socket bind, no port
	 * setup): print usage to stdout and exit 0 without starting the daemon. */
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }

	const char *pend_in = NULL, *pend_out = NULL;
	int prefill_ms = 12, cpu = 3, prio = 80; long period_ns = 250000;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--in") && i + 1 < argc) pend_in = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) pend_out = argv[++i];
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			char *spec = argv[++i], *colon = strchr(spec, ':');
			if (!colon) { fprintf(stderr, "--port needs IN:OUT, got '%s'\n", spec); return 2; }
			*colon = '\0';
			if (add_stream(spec, colon + 1)) return 2;
		}
		else if (!strcmp(argv[i], "--prefill-ms") && i + 1 < argc) prefill_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--period-ns") && i + 1 < argc) { period_ns = atol(argv[++i]); g_auto_rate = 0; }
		else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--detect-ms") && i + 1 < argc) g_detect_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--etf")) { g_etf = 1; g_bypass = 0; }
		else if (!strcmp(argv[i], "--etf-lead-ms") && i + 1 < argc) g_etf_lead_ns = atoi(argv[++i]) * 1000000;
		else if (!strcmp(argv[i], "--detect-confirm") && i + 1 < argc) g_detect_confirm = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mute-ms") && i + 1 < argc) g_mute_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--prio") && i + 1 < argc) prio = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bcast-only")) g_bcast_only = 1;
		else if (!strcmp(argv[i], "--bypass")) g_bypass = 1;
		else if (!strcmp(argv[i], "--ctrl-bypass")) g_ctrl_bypass = 1;
		else if (!strcmp(argv[i], "--servo-clamp-ppm") && i + 1 < argc) g_servo_clamp_ppm = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--adapt")) g_adapt = 1;
		else if (!strcmp(argv[i], "--adapt-margin") && i + 1 < argc) g_adapt_margin = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--adapt-min-ms") && i + 1 < argc) g_adapt_min_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--adapt-max-ms") && i + 1 < argc) g_adapt_max_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-plc")) g_plc = 0;
		else if (!strcmp(argv[i], "--no-auto-rate")) g_auto_rate = 0;
		else if (!strcmp(argv[i], "--reclaim")) g_reclaim = 1;
		else if (!strcmp(argv[i], "--clock-source") && i + 1 < argc) { const char *cs = argv[++i]; g_clock_local = (!strcmp(cs, "local") || !strcmp(cs, "local-in")); g_clock_in = !strcmp(cs, "local-in"); }
		else if (!strcmp(argv[i], "--clock-margin-ppm") && i + 1 < argc) g_clock_margin_ppm = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--inject-sine") && i + 1 < argc) { char *c = strchr(argv[++i], ':'); g_inj_slot = atoi(argv[i]); if (c) g_inj_freq = atof(c + 1); }
		else if (!strcmp(argv[i], "--inject-copy") && i + 1 < argc) { char *c = strchr(argv[++i], ':'); g_cp_src = atoi(argv[i]); if (c) g_cp_dst = atoi(c + 1); }
		else if (!strcmp(argv[i], "--pace-by-downstream")) g_pace_ds = 1;
		else if (!strcmp(argv[i], "--pll")) g_pll = 1;
		else if (!strcmp(argv[i], "--pll-fgain") && i + 1 < argc) g_pll_fgain = atof(argv[++i]);
		else if (!strcmp(argv[i], "--pll-pos-min")) g_pll_pos_min = 1;
		else if (!strcmp(argv[i], "--steady")) g_steady = 1;
		else if (!strcmp(argv[i], "--no-steady")) g_steady = 0;
		else if (!strcmp(argv[i], "--warm-start")) g_warm_start = 1;
		else if (!strcmp(argv[i], "--no-warm-start")) g_warm_start = 0;
		else if (!strcmp(argv[i], "--lockfile") && i + 1 < argc) g_lockfile = argv[++i];
		else if (!strcmp(argv[i], "--forward-only") || !strcmp(argv[i], "--no-return")) g_forward_only = 1;
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
		else {
			/* unknown flag (or a value-taking flag missing its argument): error to stderr,
			 * show usage, and exit 1 -- never silently ignore it and launch with defaults. */
			fprintf(stderr, "reac_repacer: unknown or malformed option '%s'\n\n", argv[i]);
			usage(stderr);
			return 1;
		}
	}
	if (pend_in && pend_out) add_stream(pend_in, pend_out);   /* legacy single-port form */
	/* No implicit default: launching with no port (bare, or only non-port flags) was a
	 * footgun -- it silently started a pacer on reactap.11:lan1. Require an explicit port. */
	if (n_streams == 0) {
		fprintf(stderr, "reac_repacer: no port given -- pass at least one --port IN:OUT\n\n");
		usage(stderr);
		return 1;
	}

	int prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
	if (prefill < 1) prefill = 1;
	if (prefill > RING_SZ - 65) prefill = RING_SZ - 65;   /* clamp to occ_cap-1 (rx drops at occ_cap) so the prefill barrier is always satisfiable */

	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	signal(SIGHUP, on_hup);    /* live re-read of the UCI hot params (applied in the loop) */

	/* allocate + open every port */
	for (int i = 0; i < n_streams; i++) {
		struct stream *s = &streams[i];
		s->slot_buf = calloc(RING_SZ, SLOT_SZ);
		s->slot_len = calloc(RING_SZ, sizeof(uint16_t));
		if (!s->slot_buf || !s->slot_len) { perror("calloc"); return 1; }
		s->occ_cap = RING_SZ - 64;        /* runaway safety only (wifi mode) */
		if (g_clock_local) {              /* local mode: idle at prefill = the max-latency budget; absorb bursts up to 2x */
			s->occ_cap = prefill * 2;
			if (s->occ_cap > RING_SZ - 64) s->occ_cap = RING_SZ - 64;
			if (s->occ_cap < 8) s->occ_cap = 8;
		}
		s->last_in = 0;
		if (open_iface(s->in_name, &s->IN) || open_iface(s->out_name, &s->OUT)) return 1;
		stream_resize(s, period_ns, prefill);
		s->occ_min_win = 1 << 30;
	}
	mlockall(MCL_CURRENT | MCL_FUTURE);

	/* pin each rx reader to its own core, NEVER the RT pacer's core (cpu) and -- when there is
	 * room -- not the eth-IRQ core 0, so the SCHED_FIFO pacer cannot preempt a reader mid-burst
	 * and overflow its socket (that preemption was the asymmetric per-port ingest "loss"). */
	int ncores = (int)sysconf(_SC_NPROCESSORS_ONLN); if (ncores < 1) ncores = 1;
	int rxc[8], nc = 0;
	for (int c = 0; c < ncores && nc < 8; c++) {
		if (c == cpu) continue;
		if (ncores > 2 && c == 0) continue;
		rxc[nc++] = c;
	}
	if (nc == 0) for (int c = 0; c < ncores && nc < 8; c++) { if (c != cpu) rxc[nc++] = c; }
	if (nc == 0) rxc[nc++] = cpu;
	for (int i = 0; i < n_streams; i++) streams[i].rx_cpu = rxc[i % nc];

	for (int i = 0; i < n_streams; i++) {
		pthread_create(&streams[i].rx_th, NULL, fwd_rx, &streams[i]);
		if (!g_forward_only) pthread_create(&streams[i].ret_th, NULL, ret_relay, &streams[i]);
	}
	if (g_clock_local) {   /* drop-immune frame-counter meter, OFF the pacer core, drain+sleep */
		g_meter_cpu = (cpu == 0) ? 1 : 0;
		pthread_t mt; pthread_create(&mt, NULL, clk_meter, &streams[0]);
	}
	if (g_cp_src >= 0 && g_cp_dst >= 0 && !g_pace_ds) {   /* TEST relay (NOT with pace-ds: both would read OUT.fd) */
		pthread_t ct; pthread_create(&ct, NULL, inj_copy_rx, &streams[0]);
	}
	if (g_pace_ds) {   /* one listener on port 0's wired side: the desk's slot grid is common */
		g_ds_cpu = cpu;
		pthread_t dt; pthread_create(&dt, NULL, ds_pacer, &streams[0]);
	}

	/* one RT pacing thread (this one) on a dedicated core, isolated from the NIC IRQ core */
	cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set); sched_setaffinity(0, sizeof set, &set);
	struct sched_param sp = { .sched_priority = prio };
	if (syscall(__NR_sched_setscheduler, 0, SCHED_FIFO, &sp) != 0) perror("sched_setscheduler");
	int pm = open("/dev/cpu_dma_latency", O_WRONLY);
	if (pm >= 0) { int z = 0; if (write(pm, &z, sizeof z) < 0) perror("cpu_dma_latency"); }

	if (g_auto_rate) {
		/* Detect the sample rate from the input packet rate: a REAC stream emits one
		 * frame per slot, so the busiest port's measured pps gives the slot period.
		 * Snap to the nearest standard rate when close, else use the measured value. */
		unsigned long long base[MAX_STREAMS];
		for (int i = 0; i < n_streams; i++) base[i] = streams[i].g_in_frames;
		long long t0 = ns_now();
		struct timespec ms = { 2, 0 }; nanosleep(&ms, NULL);
		double secs = (double)(ns_now() - t0) / 1.0e9;
		double pps = measure_pps(base, secs);
		if (pps > 100.0) {
			long snap = nearest_std_pps(pps);
			double err = pps - (double)snap; if (err < 0) err = -err;
			double chosen = (err < (double)snap * 0.08) ? (double)snap : pps;
			period_ns = (long)(1.0e9 / pps + 0.5);    /* freeze at the MEASURED rate: this IS the master's
			                                            * true frame rate (~8011-8029 @96k, NOT exactly 8000),
			                                            * so the de-jitter emit matches the producer and the
			                                            * ring never drifts. nearest_std (chosen) is only a
			                                            * family label + sanity gate, never the emit clock. */
			prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
			if (prefill < 1) prefill = 1;
			if (prefill > RING_SZ - 65) prefill = RING_SZ - 65;   /* clamp to occ_cap-1 (rx drops at occ_cap) so the prefill barrier is always satisfiable */
			for (int i = 0; i < n_streams; i++) stream_resize(&streams[i], period_ns, prefill);
			fprintf(stderr, "auto-rate: %.0f pps measured (%.1f kHz), period frozen at %ld ns\n",
			        pps, chosen * 12.0 / 1000.0, period_ns);
		} else {
			fprintf(stderr, "auto-rate: no input seen on any port; keeping period=%ld ns\n", period_ns);
		}
		/* drop whatever overfilled during the measurement window -> prefill fresh */
		for (int i = 0; i < n_streams; i++)
			__atomic_store_n(&streams[i].r_head, __atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
	}

	fprintf(stderr, "repacer(multiport): %d port(s) prefill=%d (%d ms) period=%ld ns cpu=%d bcast_only=%d fwd_only=%d adapt=%d(%d-%dms) plc=%d reclaim=%d steady=%d warm=%d clk=%s\n",
	        n_streams, prefill, prefill_ms, period_ns, cpu, g_bcast_only, g_forward_only, g_adapt, g_adapt_min_ms, g_adapt_max_ms, g_plc, g_reclaim, g_steady, g_warm_start,
	        g_clock_local ? "local" : "wifi");
	for (int i = 0; i < n_streams; i++)
		fprintf(stderr, "  port %d: %s -> %s\n", i, streams[i].in_name, streams[i].out_name);

	/* INGEST SETTLE: at daemon start the WDS/zone delivery ramps -- one zone can lag the other
	 * by ~1600 fps for ~2 s (measured), a startup loss that drains that port to empty and, under
	 * the shared clock, FREEZES a delay offset (the "B behind" spread). Wait until every active
	 * port's input rate is full + matched across ports for 2 consecutive seconds, DISCARDING input
	 * meanwhile, so the prefill barrier below then fills both ports cleanly + equally and they lock
	 * in unison. Uses g_in_frames (true ingest, counted before the ring) so discarding is free. */
	{
		unsigned long long base[MAX_STREAMS];
		long long t0 = ns_now();
		for (int i = 0; i < n_streams; i++) base[i] = streams[i].g_in_frames;
		int good = 0, settle_iter = 0;
		double matched_pps = 0;
		while (running && good < 2 && settle_iter < 30) {   /* cap ~30 s so a single-port rig still starts */
			struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); settle_iter++;
			long long now = ns_now(); double secs = (double)(now - t0) / 1.0e9; t0 = now;
			double mx = 0, mn = 1e18; int any = 0;
			for (int i = 0; i < n_streams; i++) {
				unsigned long long d = streams[i].g_in_frames - base[i];
				base[i] = streams[i].g_in_frames;
				double r = secs > 0 ? (double)d / secs : 0;
				if (r < 100.0) continue;                 /* idle/dormant port: not required */
				any = 1;
				if (r > mx) mx = r;
				if (r < mn) mn = r;
			}
			if (any && mx > 0 && (mx - mn) / mx < 0.01) { good++; matched_pps = mx; }  /* matched within 1%; fastest port = least-lossy = master rate */
			else good = 0;
			for (int i = 0; i < n_streams; i++)          /* discard the ramp so rings start fresh after it */
				__atomic_store_n(&streams[i].r_head, __atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
		}
		/* now that ingest is stable+matched, take an ACCURATE rate over ~4 s (a single 1 s settle
		 * delta is too noisy -> a low second sets the period too slow -> occ fills deep). Average the
		 * fastest (least-lossy) port = the master rate. */
		if (g_auto_rate && running && matched_pps > 100.0) {
			unsigned long long b2[MAX_STREAMS]; long long tm = ns_now();
			for (int i = 0; i < n_streams; i++) b2[i] = streams[i].g_in_frames;
			struct timespec ms4 = { 4, 0 }; nanosleep(&ms4, NULL);
			double secs2 = (double)(ns_now() - tm) / 1.0e9, best = 0;
			for (int i = 0; i < n_streams; i++) {
				double r = secs2 > 0 ? (double)(streams[i].g_in_frames - b2[i]) / secs2 : 0;
				if (r > best) best = r;
			}
			if (best > 100.0) matched_pps = best;
			for (int i = 0; i < n_streams; i++)   /* discard the measurement fill -> rings fresh for the barrier */
				__atomic_store_n(&streams[i].r_head, __atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
		}
		/* Use the settle's multi-second rate measurement to set the emit clock -- far more accurate
		 * than the 2 s auto-rate seed, so occ holds at prefill with NO convergence fill = controlled
		 * latency. Auto mode only (a forced --period-ns is respected). The PLL then trims the residual. */
		if (g_auto_rate && matched_pps > 100.0) {
			/* seed the EXACT standard frame rate (96k=8000 fps), not the bursty WDS measurement --
			 * any window over the jittery delivery scatters +/- several % (a correlated both-ports dip
			 * even passes the matched check) -> a wrong absolute seed bloats to the ring cap or drains.
			 * The master is within ~0.05% of the standard; the PLL nulls that tiny residual exactly. */
			long std = nearest_std_pps(matched_pps);
			double seed = (matched_pps > (double)std * 0.85 && matched_pps < (double)std * 1.15) ? (double)std : matched_pps;  /* wide window: the RETURN path measures up to ~10% low (WDS loss) but its true rate is still the master standard */
			period_ns = (long)(1.0e9 / seed + 0.5);
			prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
			if (prefill < 1) prefill = 1;
			if (prefill > RING_SZ - 65) prefill = RING_SZ - 65;
			for (int i = 0; i < n_streams; i++) stream_resize(&streams[i], period_ns, prefill);
		}
		/* PLL warm-start: if a converged lock for one of our ports was persisted at this same
		 * sample rate, seed the base period from it so the daemon starts ALREADY LOCKED instead
		 * of spending minutes audibly converging. The saved value already carries this router's
		 * crystal offset (per-port keyed); the PLL then slow-tracks it for long-term drift. */
		if (g_auto_rate && g_warm_start && matched_pps > 100.0) {
			long rate_hz = rate_label_hz(period_ns);
			long warm = 0;
			for (int i = 0; i < n_streams && !warm; i++) warm = lock_load(streams[i].out_name, rate_hz);
			if (warm > 0) {
				period_ns = warm;
				prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
				if (prefill < 1) prefill = 1;
				if (prefill > RING_SZ - 65) prefill = RING_SZ - 65;
				for (int i = 0; i < n_streams; i++) stream_resize(&streams[i], period_ns, prefill);
				fprintf(stderr, "warm-start: restored converged period=%ld ns (%ld Hz) from %s\n",
				        period_ns, rate_hz, g_lockfile);
			}
		}
		fprintf(stderr, "repacer: ingest settled in %d s, rate=%.1f pps, period=%ld ns -> filling\n",
		        settle_iter, matched_pps, period_ns);
	}

	/* pace once EVERY actively-receiving port has prefilled (not just the first) -- otherwise
	 * a port that fills slower than its peers starts pacing under-filled and underruns at
	 * startup. Ports with no input yet (n_rx==0) don't block the start; they join cleanly
	 * when their data arrives. */
	while (running) {
		int any_active = 0, all_ready = 1;
		for (int i = 0; i < n_streams; i++) {
			if (streams[i].n_rx == 0) continue;          /* idle port: not required to start */
			any_active = 1;
			unsigned occ = (__atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE) - streams[i].r_head) & RING_MASK;
			if ((int)occ < prefill) all_ready = 0;
		}
		if (any_active && all_ready) break;
		struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
	}
	/* UNISON occupancy-anchored start: snapshot every active port's ring occupancy at ONE
	 * instant and set the COMMON start floor = max(prefill, max occ across active ports). Every
	 * port then gates its FIRST emit on eq_floor (shallow ports DEFER -> add buffer, never drop),
	 * so all ports begin at the SAME depth = the SAME output delay. Pure wall-clock OCCUPANCY
	 * (per-zone REAC counters are independent sequences). Anchoring everyone UP to the deepest
	 * honours "raise the buffer for ALL" (one common safe window >= --prefill-ms). */
	int eq_floor = prefill;       /* common start occupancy (frames); >= prefill = --prefill-ms */
	int eq_done  = 0;             /* latched once all initially-active ports reach the floor + begin */
	for (int i = 0; i < n_streams; i++) {
		if (streams[i].n_rx == 0) continue;
		int occ = (int)((__atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE)
		                 - streams[i].r_head) & RING_MASK);
		if (occ > eq_floor) eq_floor = occ;   /* anchor everyone UP to the deepest */
	}
	if (eq_floor > streams[0].occ_cap - 1) eq_floor = streams[0].occ_cap - 1;
	fprintf(stderr, "repacer: prefilled, pacing (eq_floor=%d frames = %d ms)\n",
	        eq_floor, (int)((long long)eq_floor * period_ns / 1000000));

	long long deadline = ns_now() + period_ns; double period = (double)period_ns;
	if (g_etf) { struct timespec ta, tm; clock_gettime(CLOCK_TAI, &ta); clock_gettime(CLOCK_MONOTONIC, &tm);
	            g_tai_off = (ta.tv_sec - tm.tv_sec) * 1000000000LL + (ta.tv_nsec - tm.tv_nsec); }
	long long last = ns_now();
	/* wired-clock state (--clock-source local): base_per is the emit base period as a DOUBLE
	 * (sub-ns rate resolution: 1 ns of period at 96 k is 8 ppm -- integer ns alone quantizes the
	 * clock audibly over hours). dl_carry accumulates the fractional ns so the emitted cadence
	 * is exactly base_per on average. clk_act latches once the meter is valid; while latched the
	 * occupancy-chasing PLL below is bypassed (occupancy carries WiFi noise; the wire carries
	 * the clock). Meter lost > 30 s -> unlatch, PLL resumes (graceful wifi fallback). */
	double base_per = (double)period_ns, dl_carry = 0.0;
	int clk_act = 0; long long clk_last_ok = g_clock_local ? ns_now() : 0;
	double ph_ema = 0.0, ph_int = 0.0, ph_freq_adj = 0.0;   /* slot-grid PI phase PLL state (--pace-by-downstream) */
	long lock_saved_per = 0;
	/* wired-meter state: cumulative anchor (frame #1 of the current continuous stream) and
	 * an online (Welford) fit over EVERY 1 Hz (count, time) observation since it -- still
	 * "total frames / elapsed time since frame #1", but using all the evidence: a single
	 * endpoint's ~ms read jitter swings a 2-point estimate by tens of ppm for minutes,
	 * while the all-samples fit kills it as T*sqrt(N). The window only grows. */
	unsigned long long met_n0 = 0, met_prev_n = 0;
	long long met_t0 = 0, met_prev_t = 0, met_prev_y = 0;
	double met_mx = 0, met_my = 0, met_c = 0, met_m2 = 0; long long met_ns = 0, met_age = 0;
	/* drain servo: bias the one shared clock a few hundred ppm to steer the TIGHTEST active
	 * port's smoothed occupancy onto its (adaptive) target -- fast to drain excess latency,
	 * slow to fill, gated on the tightest port so none underruns. A steady, slowly-ramped
	 * sub-cent offset is inaudible (unlike the old fast-wandering servo), and no frame is
	 * ever dropped, so reclaiming latency from a clean signal stays click-free. PI with
	 * anti-windup; clamp = --servo-clamp-ppm. */
	double bias_ppm = 0.0, servo_integ = 0.0;
	double max_ppm = (double)g_servo_clamp_ppm;
	int servo_div = 0;
	int drift_div = 0; double drift_prev = 1e9;   /* base tracker: null long-term occ-vs-target drift */
	int pll_div = 0; double pll_occ_ref = -1.0; long long pll_elapsed = 0;  /* hold-then-correct PLL: occ baseline + ticks since last trim (64-bit: >74h hold on 32-bit long) */
	int pll_locked = 0;                            /* 0 = cold acquire (wide clamp), 1 = converged (inaudible 30 ppm clamp) */
	int lock_div = 0;                              /* warm-start: persist the converged lock periodically */
	/* ONE shared target for all ports (they share a clock + a source). Sized to the worst
	 * low-water dip across ports + margin: grow at once to cover a burst, ease down ~1 slot/4s
	 * when calm. The servo then steers the tightest port's smoothed occupancy onto it. */
	double shtgt = (double)prefill;
	int adapt_div = 0;
	int t_floor = (int)((long long)g_adapt_min_ms * 1000000 / period_ns); if (t_floor < 4) t_floor = 4;
	int t_ceil  = (int)((long long)g_adapt_max_ms * 1000000 / period_ns); if (t_ceil > RING_SZ - 64) t_ceil = RING_SZ - 64;
	int rate_chg = 0;

	/* glitch-free occupancy retarget state: while retarget_ticks>0 the period carries an
	 * extra retarget_ppm bias that walks the live occupancy to a newly-set depth (no flush,
	 * no drop). Set by apply_hot_params; consumed + decremented in the tick below. */
	double retarget_ppm = 0.0; int retarget_ticks = 0;

	/* expose the loop's tuning state to the live-reconfig path (ubus set / SIGHUP); both run
	 * on this thread, so they mutate it directly through these pointers. prefill_ms tracks the
	 * live target depth so get reports it and a fresh set can compute its delta. */
	struct pacer_live live = {
		.prefill_ms = prefill_ms, .period_ns = period_ns,
		.prefill = &prefill, .shtgt = &shtgt, .t_floor = &t_floor, .t_ceil = &t_ceil,
		.max_ppm = &max_ppm, .retarget_ppm = &retarget_ppm, .retarget_ticks = &retarget_ticks,
	};
	int ubus_fd = ubus_setup(&live);
	int applied_gen = reconf_gen;

	while (running) {
		long long wake = g_etf ? deadline - g_etf_lead_ns : deadline;   /* ETF: wake loose, kernel times the egress */
		struct timespec d = { wake / 1000000000LL, wake % 1000000000LL };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);
		g_cur_txtime = g_etf ? deadline + g_tai_off : 0;

		/* emit every port on this shared tick. Pre eq_done shallow ports DEFER to the common
		 * floor; post eq_done a late/idle hot-joiner gates on the peers' live running depth so it
		 * joins delay-matched. With --servo-clamp-ppm 0 shtgt stays == prefill. */
		long long now_pll = ns_now();
		int join_depth = (int)(shtgt + SERVO_SETPOINT + 0.5);
		for (int i = 0; i < n_streams; i++) {
			int active = 0;
			pace_one(&streams[i], &active, eq_floor, eq_done, join_depth);
		}

		/* service the two live-reconfig triggers AFTER the emit (the emit is the jitter-critical
		 * part; the apply takes effect from the next tick). ubus set stages into g_pending + bumps
		 * reconf_gen; SIGHUP re-reads UCI here (async-safe -- the handler only set a flag). Both
		 * funnel through apply_hot_params -> the shared glitch-free retarget. */
		ubus_service(ubus_fd);
		if (got_hup) { got_hup = 0; reload_hot_params_from_uci(&live); applied_gen = reconf_gen; }
		if (reconf_gen != applied_gen) { applied_gen = reconf_gen; apply_hot_params(&live, &g_pending); }
		/* latch the occupancy-anchored start once every initially-active port has begun. ONE-TIME;
		 * only re-armed on a rate-change relock. This also un-freezes the PLL below. */
		if (!eq_done) {
			int all_started = 1, any = 0;
			for (int i = 0; i < n_streams; i++) {
				if (streams[i].n_rx == 0) continue;
				any = 1;
				if (!streams[i].started) all_started = 0;
			}
			if (any && all_started) eq_done = 1;
		}
		/* tightest smoothed occupancy across active ports -> the servo input */
		double min_ema = 1e9; int have = 0;
		for (int i = 0; i < n_streams; i++) {
			if (!streams[i].started || streams[i].n_rx == 0) continue;
			have = 1;
			if (streams[i].occ_ema < min_ema) min_ema = streams[i].occ_ema;
		}
		/* shared-target adaptation (~every 1 s). Size to the worst BURST DEPTH across ports:
		 * D_i = occ_ema_i - occ_min_i is how far port i dipped below its own smoothed level
		 * this second -- offset-independent (a port sitting higher from a past underrun does
		 * not inflate it). need = max(D) + margin. Grow at once to cover a bigger burst;
		 * ease toward the requirement exponentially when calmer (reclaim latency, no drops). */
		if (g_adapt && have && ++adapt_div >= 4000) {
			double maxD = 0;
			for (int i = 0; i < n_streams; i++) {
				if (!streams[i].started || streams[i].n_rx == 0) continue;
				double D = streams[i].occ_ema - (double)streams[i].occ_min_win;
				if (D > maxD) maxD = D;
			}
			double need = maxD + (double)g_adapt_margin;
			if (need > shtgt) shtgt = need;                /* cover a bigger burst immediately */
			else shtgt += (need - shtgt) * 0.02;           /* slow-decaying peak-hold: ease down ~2 %/s,
			                                                * so the target stays above the worst burst
			                                                * seen in the last ~30 s (a 1 s window alone
			                                                * misses bursts that fall in calmer seconds) */
			if (shtgt < (double)t_floor) shtgt = (double)t_floor;
			if (shtgt > (double)t_ceil)  shtgt = (double)t_ceil;
			for (int i = 0; i < n_streams; i++) { streams[i].occ_min_win = 1 << 30; streams[i].target = (int)shtgt; }
			adapt_div = 0;
		}
		/* drain servo (~every 16 ms): steer the tightest port's occupancy onto the shared target.
		 * STEADY MODE keeps this OFF the emit path entirely -- a per-tick rate nudge is exactly the
		 * output-cadence jitter v2 must not emit (the stagebox recovers word clock from the emit IFI).
		 * Short-term occupancy swings are absorbed by the ring depth and trimmed only glacially by the
		 * PLL below; bias_ppm stays 0 so the emit period free-runs at the (PLL-trimmed) base. */
		if (!g_steady && have && ++servo_div >= 64) {
			double dpos = (min_ema - shtgt) - SERVO_SETPOINT;   /* >0: even the tightest port has slack -> drain */
			servo_integ += dpos;
			double raw = SERVO_KP * dpos + SERVO_KI * servo_integ;
			if (raw >  max_ppm) { raw =  max_ppm; servo_integ -= dpos; }   /* anti-windup */
			else if (raw < -max_ppm) { raw = -max_ppm; servo_integ -= dpos; }
			bias_ppm = raw;
			period = (double)period_ns * (1.0 - bias_ppm * 1e-6);          /* +bias = clock fast = drain */
			servo_div = 0;
		}

		/* base-rate tracker (slow): shift the frozen base period ONLY when the fast servo is
		 * RAILED and occ is STILL drifting away from target in the rail's direction -- i.e. the
		 * servo is maxed but losing, which only happens when the base rate is off by more than the
		 * clamp. This ignores the startup/reclaim drain (servo railed but occ moving TOWARD target)
		 * and fast jitter. The fast servo (16 ms) handles jitter; this (5 s) corrects the systematic
		 * clock offset the 2 s startup estimate missed. Self-stopping; auto-rate only. */
		if (g_auto_rate && max_ppm > 0.5 && have && ++drift_div >= 40000) {
			double rel = min_ema - shtgt;
			if (drift_prev < 1e8) {
				double dd = rel - drift_prev;                          /* occ-vs-target drift over the window */
				if (bias_ppm >= max_ppm - 1.0 && dd > 2.0)            /* servo maxed-draining yet occ STILL rising -> base too slow */
					period_ns = (long)((double)period_ns * (1.0 - 200e-6) + 0.5);
				else if (bias_ppm <= -max_ppm + 1.0 && dd < -2.0)     /* servo maxed-filling yet occ STILL falling -> base too fast */
					period_ns = (long)((double)period_ns * (1.0 + 200e-6) + 0.5);
			}
			drift_prev = rel; drift_div = 0;
		}
		/* hold-then-correct frequency lock -- the long-term rate lock AND, in steady mode, the ONLY
		 * path occupancy is allowed to touch the clock. The period is HELD CONSTANT and re-trimmed
		 * only when the buffer has drifted >= PLL_TRIG_FRAMES since the last trim (a real rate error,
		 * measured over a long baseline so it sits well above per-window occupancy noise) or occ has
		 * wandered outside PLL_POS_BAND of target (rail safety). The trim nulls the measured rate
		 * error (drift / elapsed ticks), clamped to PLL_MAXPPM so even the rare correction is inaudible.
		 * Steady mode runs this even without --pll (it IS steady mode's occupancy trim); short-term WDS
		 * bursts show as occupancy swings on occ_ema that stay inside the band and never reach the rate. */
		/* wire-clock mode: the occupancy PLL is OFF the moment the meter is alive (acquiring
		 * OR latched) -- two correctors on one period beat against each other (a trim every
		 * ~4 s PLL window = a beep at constant pace). It returns only as the >30 s-dead
		 * meter fallback. */
		/* once the wire rate is SET, nothing else ever touches the clock again -- a
		 * quiet meter means FREE-RUN on the last set value (near-exact; the margin
		 * re-derive picks up when the meter returns). The PLL exists only as the
		 * never-acquired fallback (no REAC on the wire within 30 s of start). */
		int clk_alive = g_clock_local && (clk_act || (clk_last_ok && now_pll - clk_last_ok < 30000000000LL));
		if (!clk_alive && (g_pll || g_steady) && eq_done && have && ++pll_div >= PLL_WIN) {
			double msum = 0; int mc = 0;
			for (int i = 0; i < n_streams; i++)
				if (streams[i].started && streams[i].n_rx) { msum += streams[i].occ_ema; mc++; }
			if (mc) {
				double mean_occ = msum / mc;
				pll_elapsed += PLL_WIN;
				if (pll_occ_ref < 0) { pll_occ_ref = mean_occ; pll_elapsed = 0; }  /* first window: set baseline + zero elapsed so drift/elapsed is exact from the next window (no off-by-one) */
				double drift = mean_occ - pll_occ_ref;                        /* buffer movement since the last trim */
				double pos   = g_pll_pos_min ? min_ema : mean_occ;            /* position source: tightest port or mean depth */
				double err_t = pos - shtgt;                                   /* distance from the ideal target depth */
				if ((drift >= PLL_TRIG_FRAMES || drift <= -PLL_TRIG_FRAMES ||
				     err_t >= PLL_POS_BAND   || err_t <= -PLL_POS_BAND) && pll_elapsed > 0) {
					double adj = -(drift / (double)pll_elapsed) * 1e6 * g_pll_fgain;   /* rate-lock: null the measured rate error */
					/* OFF-BAND occupancy recovery: a WDS-stall drain (or cold start) left occ far from target.
					 * The gentle rate-lock alone would take ~minutes to refill a deep deficit, sitting near a
					 * rail meanwhile. Add a proportional pull that steers occ back over ~PLL_RECOVER_SECS and
					 * widen the clamp -- gentle/sub-cent for a small excursion, ramping to the 500 ppm clamp only
					 * for a deep drain. (err_t<0 = too empty -> +adj = longer period = slower = fills; symmetric.) */
					int off_band = (err_t >= PLL_POS_BAND || err_t <= -PLL_POS_BAND);
					if (off_band) adj += -err_t / (PLL_RECOVER_SECS * 8000.0) * 1e6;
					/* LOCK detection: only IN-BAND (an undisturbed steady state) AND once the required (raw,
					 * pre-clamp) rate correction already fits within the inaudible clamp -> latch locked and stay
					 * in the 30 ppm regime. (Detect on correction magnitude, NOT drift-within-window, which is
					 * small by construction between triggers and would latch on the very first window.) */
					if (!off_band && adj <= PLL_MAXPPM && adj >= -PLL_MAXPPM) pll_locked = 1;
					/* regime clamp: 30 ppm (inaudible) only when LOCKED and IN-BAND; wide (500 ppm) while
					 * acquiring OR recovering off-band -- both regimes where the buffer is not yet a stable
					 * pitch reference, so a larger step is acceptable and gets us safe fast. */
					double clamp = (pll_locked && !off_band) ? PLL_MAXPPM : PLL_ACQ_PPM;
					if (adj >  clamp) adj =  clamp;
					else if (adj < -clamp) adj = -clamp;
					period_ns = (long)((double)period_ns * (1.0 + adj * 1e-6) + 0.5);
					base_per = (double)period_ns;
					period = base_per * (1.0 - bias_ppm * 1e-6);   /* steady: bias_ppm==0 -> period==base */
					pll_occ_ref = mean_occ;                                  /* re-baseline the hold point */
					pll_elapsed = 0;
				}
			}
			pll_div = 0;
		}
		/* warm-start persist (~every 30 s once converged): write the live base period out per port so
		 * the NEXT start (restart or deploy) seeds the PLL already-locked. Gated on pll_locked so we only
		 * ever persist a CONVERGED period (never a pre-lock transient), plus eq_done + no in-flight depth
		 * walk; the file write is off the jitter-critical emit. */
		if (g_warm_start && (pll_locked || clk_act) && eq_done && retarget_ticks == 0 && have && ++lock_div >= LOCK_SAVE_WIN) {
			if (period_ns != lock_saved_per) { lock_save(period_ns); lock_saved_per = period_ns; }   /* flash write on the RT thread: only when changed */
			lock_div = 0;
		}
		/* glitch-free occupancy retarget: while a depth change is walking in, bias THIS tick's
		 * period by retarget_ppm (on top of the servo/PLL period) so the occupancy slides toward
		 * the new target without a flush. The walk is bounded + short, then it hands the steady
		 * state back to the servo/PLL at the new target. emit_period is per-tick; period (the base)
		 * is left untouched so the loop's own control is undisturbed when the walk ends. */
		double emit_period = period;
		if (retarget_ticks > 0) {
			emit_period = period * (1.0 - retarget_ppm * 1e-6);   /* +ppm = faster = drain/shrink */
			retarget_ticks--;
			if (retarget_ticks == 0) retarget_ppm = 0.0;
		}
		emit_period *= (1.0 + ph_freq_adj * 1e-6);    /* slot-phase PLL: smooth freq trim, NOT a per-tick jump */
		dl_carry += emit_period;                       /* fractional-ns accumulation: cadence = emit_period exactly */
		{ long long dstep = (long long)dl_carry; dl_carry -= (double)dstep; deadline += dstep; }
		/* slot-grid phase PLL (--pace-by-downstream): drive the emit phase onto the master's
		 * downstream slot + the ~69 us box-like answer offset. The phase error (folded into one
		 * slot) is heavily EMA-filtered to reject the per-frame grid-timestamp jitter, then fed
		 * as a small PROPORTIONAL FREQUENCY trim (ppm) -- NOT a per-tick position jump, so the
		 * cadence stays smooth (no granular). Kp gives a ~2 s walk like the real box's tracking
		 * ramp; rate is already locked by the counter clock, so no integral term is needed. */
		if (g_pace_ds) {
			long long dst = g_ds_tns;
			if (dst) {
				double per2 = period;
				double ph = fmod((double)(deadline - (dst + 69000)), per2);
				if (ph < 0) ph += per2;
				if (ph > per2 / 2) ph -= per2;
				ph_ema += (ph - ph_ema) * 0.0015;          /* slow filter: reject grid jitter */
				ph_int += ph_ema;                          /* integral: nulls the residual FREQ error (= the beat) */
				if (ph_int >  4.0e9) ph_int =  4.0e9; else if (ph_int < -4.0e9) ph_int = -4.0e9;   /* anti-windup */
				ph_freq_adj = -(ph_ema * 5.0e-4 + ph_int * 1.0e-8);   /* PI: Kp ~2 s walk, Ki nulls drift (~10 s) */
				if (ph_freq_adj > 40.0) ph_freq_adj = 40.0; else if (ph_freq_adj < -40.0) ph_freq_adj = -40.0;
			}
		}
		long long now = ns_now();
		if (now - last > (long long)g_detect_ms * 1000000LL) {
			/* continuous rate detection: a SAMPLE-RATE change shifts the busiest port's input
			 * pps far from the pacing rate (a clock offset is <0.3%, a 44.1<->48<->96 k change is
			 * 9-100%). Re-lock every port at the new rate instead of draining. */
			double rsecs = (double)(now - last) / 1.0e9;
			unsigned long long inb[MAX_STREAMS];
			for (int i = 0; i < n_streams; i++) inb[i] = streams[i].last_in;
			double in_pps = measure_pps(inb, rsecs);
			for (int i = 0; i < n_streams; i++) streams[i].last_in = streams[i].g_in_frames;
			long cand = nearest_std_pps(in_pps);
			double cerr = (in_pps >= (double)cand) ? (in_pps - cand) / cand : (cand - in_pps) / cand;
			int is_change = (g_auto_rate && in_pps > 2000.0 && cerr < 0.03 && cand != nearest_std_pps(1.0e9 / period_ns));
			if (is_change && rate_chg == 0 && g_mute_ms > 0) g_mute_until = now + (long long)g_mute_ms * 1000000LL;   /* first sign of a rate change -> mute immediately */
			if (is_change) rate_chg++; else rate_chg = 0;
			if (rate_chg >= g_detect_confirm) {   /* a standard rate sustained the detect window -> a real (rare) change */
				rate_chg = 0; period_ns = (long)(1.0e9 / in_pps + 0.5); period = (double)period_ns;  /* emit at the MEASURED rate of the new family (cand only DETECTS the change) */
				base_per = (double)period_ns; dl_carry = 0.0;
				clk_act = 0; met_n0 = 0; met_ns = 0; met_mx = met_my = met_c = met_m2 = 0;   /* wired meter: re-anchor at the new rate */
				prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
				if (prefill < 1) prefill = 1;
				if (prefill > RING_SZ - 65) prefill = RING_SZ - 65;   /* clamp to occ_cap-1 (rx drops at occ_cap) so the prefill barrier is always satisfiable */
				bias_ppm = 0; servo_integ = 0; servo_div = 0;
				shtgt = (double)prefill; adapt_div = 0; drift_prev = 1e9;
				t_floor = (int)((long long)g_adapt_min_ms * 1000000 / period_ns); if (t_floor < 4) t_floor = 4;
				t_ceil  = (int)((long long)g_adapt_max_ms * 1000000 / period_ns); if (t_ceil > RING_SZ - 64) t_ceil = RING_SZ - 64;
				for (int i = 0; i < n_streams; i++) {
					streams[i].have_last = 0;
					stream_relock(&streams[i], period_ns, prefill);
				}
				/* all-ports-armed relock barrier with stalled-port bypass: a port whose input
				 * stopped keeps n_rx!=0 but never refills, so don't wait forever on it (else all
				 * ports freeze). Bypass a port whose n_rx hasn't advanced 300 ms after entry. */
				{
					unsigned long long rb[MAX_STREAMS];
					for (int i = 0; i < n_streams; i++) rb[i] = streams[i].n_rx;
					long long t_barr = ns_now();
					while (running) {
						int any_active = 0, all_ready = 1;
						for (int i = 0; i < n_streams; i++) {
							if (streams[i].n_rx == 0) continue;
							if (streams[i].n_rx == rb[i] && ns_now() - t_barr > 300000000LL) continue;
							any_active = 1;
							int o = (int)((__atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE) - streams[i].r_head) & RING_MASK);
							if (o < prefill) all_ready = 0;
						}
						if (any_active && all_ready) break;
						struct timespec rt = { 0, 1000000 }; nanosleep(&rt, NULL);
					}
				}
				/* UNISON: re-arm the occupancy-anchor at the new rate -- re-snapshot the common floor
				 * and clear eq_done (stream_relock already set started=0) so every port re-gates on the
				 * new floor; reseed the PLL so it re-converges from the new equalized state. */
				eq_floor = prefill; eq_done = 0; pll_occ_ref = -1.0; pll_elapsed = 0; pll_locked = 0; pll_div = 0; lock_div = 0;
				for (int i = 0; i < n_streams; i++) {
					if (streams[i].n_rx == 0) continue;
					int occ = (int)((__atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE) - streams[i].r_head) & RING_MASK);
					if (occ > eq_floor) eq_floor = occ;
				}
				if (eq_floor > streams[0].occ_cap - 1) eq_floor = streams[0].occ_cap - 1;
				for (int i = 0; i < n_streams; i++) streams[i].last_in = streams[i].g_in_frames;
				/* the relock re-prefills from scratch at the new period: abandon any in-flight depth
				 * walk and re-base the live-reconfig view (prefill_ms unchanged; period_ns is new, so
				 * a later set computes its slot count correctly). */
				retarget_ticks = 0; retarget_ppm = 0.0; live.period_ns = period_ns;
				fprintf(stderr, "auto-rate: input rate changed to %.0f pps (%.1f kHz) -> re-locked all ports, period=%ld ns, eq_floor=%d\n", in_pps, in_pps * 12.0 / 1000.0, period_ns, eq_floor);
				if (g_mute_ms > 0) g_mute_until = ns_now() + (long long)g_mute_ms * 1000000LL;   /* hold silence through re-prefill */
				deadline = ns_now() + period_ns; last = ns_now(); continue;
			}
			/* --clock-source local: slew the emit base onto the WIRED port's counted rate.
			 * period = elapsed/(frames-1) measured by met_tick on the wire -- the master crystal,
			 * exact and noise-free. Steps are clamped to 50 ppm per window (sub-cent); once matched
			 * the meter only refines, so steady-state steps are sub-ppm = a dead-constant cadence.
			 * While the meter is valid the occupancy PLL above is bypassed: occ is free to swing
			 * with WDS bursts and NEVER touches the clock -- no recovery warble, no ratchet. */
			if (g_clock_local) {
				/* THE clock calculation (operator-settled): period = elapsed time / total
				 * frames, CUMULATIVE since the anchor frame, count from the driver's
				 * lossless rx_packets. The window only grows, so precision improves ~1/T
				 * without bound -- impossible to drift after a few thousand frames. The
				 * anchor resets only on a real discontinuity (iface reset, stream stall,
				 * sample-rate change). */
				int fresh = 0;
				if (now - met_prev_t >= 1000000000LL) {
					unsigned long long mn = g_met_n; long long mtl = g_met_tlast;
					if (mn < met_prev_n || (met_prev_n && mn - met_prev_n < 100)) { met_n0 = 0; met_ns = 0; met_mx = met_my = met_c = met_m2 = 0; }   /* discontinuity -> re-anchor */
					if (mn >= 2 && mtl != met_prev_y) {
						if (!met_n0) { met_n0 = mn; met_t0 = mtl; }
						double x = (double)(mn - met_n0), y = (double)(mtl - met_t0);
						met_ns++;
						double dx = x - met_mx;
						met_mx += dx / (double)met_ns;
						met_my += (y - met_my) / (double)met_ns;
						met_c  += dx * (y - met_my);
						met_m2 += dx * (x - met_mx);
						met_age = mtl - met_t0; met_prev_y = mtl;
						fresh = 1;
					}
					met_prev_n = mn; met_prev_t = now;
				}
				if (fresh && met_n0 && met_ns >= 8 && met_m2 > 0) {
					double slope = met_c / met_m2;   /* ns per frame, fitted over all samples since the anchor */
					long long age = met_age;
					double spps = 1.0e9 / slope;
					long mfam = nearest_std_pps(spps);
					long efam = nearest_std_pps(1.0e9 / (double)period_ns);
					double mferr = (spps >= (double)mfam ? spps - (double)mfam : (double)mfam - spps) / (double)mfam;
					if (mferr < 0.03 && mfam == efam) {
						/* THE PACE IS frames/time, NOTHING ELSE. base_per = the cumulative slope
						 * (total frame-counter delta / elapsed since the anchor). The trigger to
						 * RE-APPLY is ALSO frames/time: only when the cumulative estimate itself
						 * moves past a small ppm margin (it barely does -- cumulative converges as
						 * 1/N). Jitter-buffer occupancy is NEVER an input or trigger to the clock;
						 * occupancy drives ONLY ring depth. (Operator-pinned: no queue in the pace.) */
						double dppm = (slope - base_per) / base_per * 1e6;
						if (!clk_act) {
							base_per = slope;   /* converging: track the cumulative estimate */
							if (age >= 30000000000LL && dppm < (double)g_clock_margin_ppm && dppm > -(double)g_clock_margin_ppm) {
								clk_act = 1;
								fprintf(stderr, "clock: wire rate locked, period=%.2f ns (%.2f pps), holding\n", base_per, 1.0e9 / base_per);
							}
						} else if (dppm > (double)g_clock_margin_ppm || dppm < -(double)g_clock_margin_ppm) {
							fprintf(stderr, "clock: cumulative rate moved %.2f ppm, re-applied %.2f -> %.2f ns\n", dppm, base_per, slope);
							base_per = slope;
						}
						period_ns = (long)(base_per + 0.5);
						period = base_per * (1.0 - bias_ppm * 1e-6);
						clk_last_ok = now;
					} else if (mfam != efam) {
						met_n0 = 0; met_ns = 0; met_mx = met_my = met_c = met_m2 = 0;   /* rate changed -> re-anchor */
					}
				}
			}
			live.period_ns = period_ns;   /* keep the live-reconfig view current (PLL/base tracker drift the base) */
			/* per-port telemetry line */
			fprintf(stderr, "per=%.1f%s bias=%+.0fppm", period,
			        g_clock_local ? (clk_act ? " clk=wire" : " clk=acq") : "", bias_ppm);
			for (int i = 0; i < n_streams; i++) {
				struct stream *s = &streams[i];
				unsigned head = s->r_head, tail = __atomic_load_n(&s->r_tail, __ATOMIC_ACQUIRE);
				int occ = (int)((tail - head) & RING_MASK);
				fprintf(stderr, " | %s occ=%d(%dms) tgt=%d under=%llu plc=%llu rx=%llu",
				        s->out_name, occ, (int)((long long)occ * period_ns / 1000000), s->target,
				        s->n_underrun, s->n_plc, s->n_rx);
			}
			fprintf(stderr, "\n");
			last = now;
		}
	}
	/* persist the converged lock on a clean exit so a stop/start (or deploy) restarts already-locked.
	 * Gated on pll_locked: never persist a pre-convergence period (it would warm-restore a bad rate). */
	if (g_warm_start && (pll_locked || clk_act) && eq_done) lock_save(period_ns);
#ifdef HAVE_UBUS
	if (g_ubus_ctx) ubus_free(g_ubus_ctx);
#endif
	for (int i = 0; i < n_streams; i++)
		fprintf(stderr, "repacer: stop %s->%s rx=%llu tx=%llu txerr=%llu under=%llu | ret=%llu reterr=%llu\n",
		        streams[i].in_name, streams[i].out_name, streams[i].n_rx, streams[i].n_tx, streams[i].n_txerr, streams[i].n_underrun, streams[i].n_ret, streams[i].n_reterr);
	return 0;
}
