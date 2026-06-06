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
//        (single port also accepts the legacy  --in IFACE --out IFACE  form.)
//
// --forward-only: de-jitter only the forward (IN->OUT) direction and do NOT run the
//   return pass-through thread. Used on the MIXER side (IN=tunnel, OUT=mixer): the
//   upstream box->master is de-jittered, while the downstream master->box is left on
//   the kernel bridge (lossless) instead of a starvable user-space pass-through.

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
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

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

/* global config (identical for every port) */
static int g_bcast_only, g_bypass, g_ctrl_bypass, g_auto_rate = 1;
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
/* The PLL: every PLL_WIN ticks, nudge the base period to (a) zero the mean-occupancy
 * drift over the window [frequency] and (b) gently recenter occ on the target [position].
 * Each step is clamped tiny (PLL_MAXPPM) so there is NO audible rate modulation; the
 * residual halves each window until refinement gains nothing -> a constant exact rate.
 * Runs with the fast servo OFF (--servo-clamp-ppm 0), preserving constant-rate clarity. */
#define PLL_WIN     32000   /* correction window in ticks (~4 s @ 8000 fps) */
#define PLL_FGAIN   0.6     /* frequency gain: fraction of measured drift corrected per window */
#define PLL_PGAIN   0.12    /* position gain: gentle recenter toward target */
#define PLL_MAXPPM  500.0   /* max base-period step per window (ppm) -- keeps each step inaudible */
static double g_pll_fgain = PLL_FGAIN;   /* runtime override (--pll-fgain) */
static double g_pll_pgain = PLL_PGAIN;   /* runtime override (--pll-pgain); 0 = pure frequency-lock, no recenter */
static int    g_pll_pos_min = 0;         /* position-control source: 0=mean depth (default), 1=tightest port (--pll-pos-min) */

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

static long long ns_now(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

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

static long nearest_std_pps(double pps) {
	static const long t[] = { 3675, 4000, 8000 };   /* 44.1 / 48 / 96 kHz */
	long best = t[0]; double bd = 1e18;
	for (unsigned i = 0; i < 3; i++) { double d = pps - (double)t[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = t[i]; } }
	return best;
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
			if (sendto(s->OUT.fd, s->plc_buf, s->slot_len[s->last_idx], 0, (struct sockaddr *)&s->OUT.tx, sizeof s->OUT.tx) < 0) s->n_txerr++;
			else s->n_plc++;
			s->emit_ctr = (s->emit_ctr + 1) & 0xFFFF;
		}
	} else {
		uint8_t *f = s->slot_buf[head];
		if (!s->have_last) s->emit_ctr = f[14] | (f[15] << 8);                /* seed the counter */
		f[14] = s->emit_ctr & 0xFF; f[15] = (s->emit_ctr >> 8) & 0xFF;        /* own a monotonic output counter */
		if (sendto(s->OUT.fd, f, s->slot_len[head], 0, (struct sockaddr *)&s->OUT.tx, sizeof s->OUT.tx) < 0) s->n_txerr++;
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

int main(int argc, char **argv) {
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
		else if (!strcmp(argv[i], "--pll")) g_pll = 1;
		else if (!strcmp(argv[i], "--pll-fgain") && i + 1 < argc) g_pll_fgain = atof(argv[++i]);
		else if (!strcmp(argv[i], "--pll-pgain") && i + 1 < argc) g_pll_pgain = atof(argv[++i]);
		else if (!strcmp(argv[i], "--pll-pos-min")) g_pll_pos_min = 1;
		else if (!strcmp(argv[i], "--forward-only") || !strcmp(argv[i], "--no-return")) g_forward_only = 1;
	}
	if (pend_in && pend_out) add_stream(pend_in, pend_out);   /* legacy single-port form */
	if (n_streams == 0) add_stream("reactap.11", "lan1");     /* default */

	int prefill = (int)((long long)prefill_ms * 1000000 / period_ns);
	if (prefill < 1) prefill = 1;
	if (prefill > RING_SZ - 65) prefill = RING_SZ - 65;   /* clamp to occ_cap-1 (rx drops at occ_cap) so the prefill barrier is always satisfiable */

	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

	/* allocate + open every port */
	for (int i = 0; i < n_streams; i++) {
		struct stream *s = &streams[i];
		s->slot_buf = calloc(RING_SZ, SLOT_SZ);
		s->slot_len = calloc(RING_SZ, sizeof(uint16_t));
		if (!s->slot_buf || !s->slot_len) { perror("calloc"); return 1; }
		s->occ_cap = RING_SZ - 64;        /* runaway safety only */
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

	fprintf(stderr, "repacer(multiport): %d port(s) prefill=%d (%d ms) period=%ld ns cpu=%d bcast_only=%d fwd_only=%d adapt=%d(%d-%dms) plc=%d reclaim=%d\n",
	        n_streams, prefill, prefill_ms, period_ns, cpu, g_bcast_only, g_forward_only, g_adapt, g_adapt_min_ms, g_adapt_max_ms, g_plc, g_reclaim);
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
	long long last = ns_now();
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
	int pll_div = 0; double pll_ref = -1.0;        /* glacial PLL: converge base period to null occ drift */
	/* ONE shared target for all ports (they share a clock + a source). Sized to the worst
	 * low-water dip across ports + margin: grow at once to cover a burst, ease down ~1 slot/4s
	 * when calm. The servo then steers the tightest port's smoothed occupancy onto it. */
	double shtgt = (double)prefill;
	int adapt_div = 0;
	int t_floor = (int)((long long)g_adapt_min_ms * 1000000 / period_ns); if (t_floor < 4) t_floor = 4;
	int t_ceil  = (int)((long long)g_adapt_max_ms * 1000000 / period_ns); if (t_ceil > RING_SZ - 64) t_ceil = RING_SZ - 64;
	int rate_chg = 0;

	while (running) {
		struct timespec d = { deadline / 1000000000LL, deadline % 1000000000LL };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);

		/* emit every port on this shared tick. Pre eq_done shallow ports DEFER to the common
		 * floor; post eq_done a late/idle hot-joiner gates on the peers' live running depth so it
		 * joins delay-matched. With --servo-clamp-ppm 0 shtgt stays == prefill. */
		int join_depth = (int)(shtgt + SERVO_SETPOINT + 0.5);
		for (int i = 0; i < n_streams; i++) {
			int active = 0;
			pace_one(&streams[i], &active, eq_floor, eq_done, join_depth);
		}
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
		/* drain servo (~every 16 ms): steer the tightest port's occupancy onto the shared target */
		if (have && ++servo_div >= 64) {
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
		/* glacial frequency-lock (the converging PLL). Self-stopping: as the drift goes to
		 * zero the correction goes to zero -> a constant exact rate, no audible modulation. */
		if (g_pll && eq_done && have && ++pll_div >= PLL_WIN) {
			double msum = 0; int mc = 0;
			for (int i = 0; i < n_streams; i++)
				if (streams[i].started && streams[i].n_rx) { msum += streams[i].occ_ema; mc++; }
			if (mc) {
				double mocc = msum / mc;
				if (pll_ref < 0) pll_ref = mocc;
				double docc = mocc - pll_ref;                                   /* drift over the window */
				double pos  = g_pll_pos_min ? min_ema : mocc;                          /* recenter source: tightest port or mean depth */
				double adj  = -(docc / (double)PLL_WIN) * 1e6 * g_pll_fgain              /* freq: null the (parallel) mean drift */
				              - ((pos - shtgt) / (double)PLL_WIN) * 1e6 * g_pll_pgain;   /* pos: gentle recenter toward the safe window (rule 5; clean equal start makes mean-recenter drain both equally) */
				if (adj >  PLL_MAXPPM) adj =  PLL_MAXPPM;
				else if (adj < -PLL_MAXPPM) adj = -PLL_MAXPPM;
				period_ns = (long)((double)period_ns * (1.0 + adj * 1e-6) + 0.5);
				period = (double)period_ns * (1.0 - bias_ppm * 1e-6);
				pll_ref = mocc;
			}
			pll_div = 0;
		}
		deadline += (long long)(period + 0.5);
		long long now = ns_now();
		if (now - last > 1000000000LL) {
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
			if (is_change) rate_chg++; else rate_chg = 0;
			if (rate_chg >= 3) {   /* a standard rate sustained ~3 s -> a real (rare) change */
				rate_chg = 0; period_ns = (long)(1.0e9 / in_pps + 0.5); period = (double)period_ns;  /* emit at the MEASURED rate of the new family (cand only DETECTS the change) */
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
				eq_floor = prefill; eq_done = 0; pll_ref = -1.0; pll_div = 0;
				for (int i = 0; i < n_streams; i++) {
					if (streams[i].n_rx == 0) continue;
					int occ = (int)((__atomic_load_n(&streams[i].r_tail, __ATOMIC_ACQUIRE) - streams[i].r_head) & RING_MASK);
					if (occ > eq_floor) eq_floor = occ;
				}
				if (eq_floor > streams[0].occ_cap - 1) eq_floor = streams[0].occ_cap - 1;
				for (int i = 0; i < n_streams; i++) streams[i].last_in = streams[i].g_in_frames;
				fprintf(stderr, "auto-rate: input rate changed to %.0f pps (%.1f kHz) -> re-locked all ports, period=%ld ns, eq_floor=%d\n", in_pps, in_pps * 12.0 / 1000.0, period_ns, eq_floor);
				deadline = ns_now() + period_ns; last = ns_now(); continue;
			}
			/* per-port telemetry line */
			fprintf(stderr, "per=%.0f bias=%+.0fppm", period, bias_ppm);
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
	for (int i = 0; i < n_streams; i++)
		fprintf(stderr, "repacer: stop %s->%s rx=%llu tx=%llu txerr=%llu under=%llu | ret=%llu reterr=%llu\n",
		        streams[i].in_name, streams[i].out_name, streams[i].n_rx, streams[i].n_tx, streams[i].n_txerr, streams[i].n_underrun, streams[i].n_ret, streams[i].n_reterr);
	return 0;
}
