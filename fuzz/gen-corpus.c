// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Writes the seed corpus of fuzz/ (make fuzz-corpus), from the frame law and the
 * two grammars, never from a capture or a network: fuzz/corpus/<harness>/<name>.
 * Each seed a check's record names is written here under that name, beside the
 * frames a rig sends (an S-0808, S-1608 and S-4000 return, the 40-channel
 * downstream broadcast, the control frames, a handshake) and the lock and uci
 * texts the daemon writes and reads. The same run writes the same bytes.
 *
 * Usage: gen-corpus <directory> */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *g_root;
static int g_fail;

static void dir(const char *h)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", g_root, h);
	if (mkdir(g_root, 0755) && errno != EEXIST)
		g_fail = 1;
	if (mkdir(p, 0755) && errno != EEXIST)
		g_fail = 1;
}

static void put(const char *h, const char *name, const uint8_t *b, size_t n)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s/%s", g_root, h, name);
	FILE *f = fopen(p, "wb");
	if (!f || fwrite(b, 1, n, f) != n || fclose(f)) {
		fprintf(stderr, "gen-corpus: cannot write %s\n", p);
		g_fail = 1;
	}
}

/* ---- frames ---------------------------------------------------------------- */

enum { AUDIO, CTRL_CDEA, CTRL_CFEA, CTRL_00EA, NOT_REAC };

/* A frame of n bytes: the master's broadcast destination, a source MAC, the REAC
 * EtherType (or IPv4 for NOT_REAC), the counter, the type, the audio region filled
 * with a pattern whose bytes all differ from their neighbours and carry the sign
 * bit often, and the C2 EA end marker where the frame law puts it. With tag, an
 * 802.1Q tag (VID 11) before the EtherType, four bytes more. */
static size_t frame(uint8_t *f, size_t n, int kind, unsigned ctr, int tag)
{
	size_t at = tag ? 4 : 0;
	memset(f, 0, n + at);
	memset(f, 0xff, 6);
	const uint8_t src[6] = { 0x00, 0x40, 0xab, 0x12, 0x34, 0x56 };
	memcpy(f + 6, src, 6);
	if (tag) {
		f[12] = 0x81; f[13] = 0x00; f[14] = 0x00; f[15] = 0x0b;
	}
	f[at + 12] = kind == NOT_REAC ? 0x08 : 0x88;
	f[at + 13] = kind == NOT_REAC ? 0x00 : 0x19;
	if (n > at + 14) f[at + 14] = (uint8_t)ctr;
	if (n > at + 15) f[at + 15] = (uint8_t)(ctr >> 8);
	const uint8_t types[5][2] = { { 0x00, 0x00 }, { 0xcd, 0xea }, { 0xcf, 0xea }, { 0x00, 0xea }, { 0x00, 0x00 } };
	if (n > at + 16) f[at + 16] = types[kind][0];
	if (n > at + 17) f[at + 17] = types[kind][1];
	for (size_t i = 18; i + at < n + at && i < n; i++)
		f[at + i] = (uint8_t)(i * 37 + 11 + (i >> 3) * 101);
	/* the end marker after the audio region of a legal width */
	for (int nch = 2; nch <= 40; nch += 2) {
		size_t clean = 52 + (size_t)nch * 36;
		if (n == clean || n == clean + 2) {
			f[at + clean - 2] = 0xc2;
			f[at + clean - 1] = 0xea;
		}
	}
	return n + at;
}

/* fuzz_wire: one byte (bit 0 --bcast-only, the rest the slot plus 2), the frame */
static void wire(const char *name, int slot, int bcast, size_t n, int kind, unsigned ctr, int tag)
{
	static uint8_t b[4096];
	b[0] = (uint8_t)(((slot + 2) << 1) | bcast);
	size_t len = frame(b + 1, n, kind, ctr, tag);
	put("wire", name, b, len + 1);
}

/* fuzz_egress: the slot plus 2, the counter, 12 samples, the frame */
static void egress(const char *name, int slot, uint32_t ctr, const int32_t v[12], size_t n, int kind,
                   unsigned fctr)
{
	static uint8_t b[4096];
	b[0] = (uint8_t)(slot + 2);
	for (int i = 0; i < 4; i++)
		b[1 + i] = (uint8_t)(ctr >> (8 * i));
	for (int s = 0; s < 12; s++)
		for (int i = 0; i < 4; i++)
			b[5 + 4 * s + i] = (uint8_t)((uint32_t)v[s] >> (8 * i));
	size_t len = frame(b + 53, n, kind, fctr, 0);
	put("egress", name, b, len + 53);
}

/* ---- the lock and the uci replies ------------------------------------------- */

/* fuzz_lock: the rate (0 44100, 1 48000, 2 96000, 3 88200, 4 none), the port's
 * length, the port, the text */
static void lock(const char *name, int rate, const char *port, const char *text, size_t n)
{
	static uint8_t b[70000];
	size_t k = strlen(port);
	b[0] = (uint8_t)rate;
	b[1] = (uint8_t)k;
	memcpy(b + 2, port, k);
	memcpy(b + 2 + k, text, n);
	put("lock", name, b, 2 + k + n);
}

static void locks(const char *name, int rate, const char *port, const char *text)
{
	lock(name, rate, port, text, strlen(text));
}

/* fuzz_uci: the ring (0 the packaged 2048 slots, 1 the source's 16384), then for
 * each of prefill_ms, adapt_min_ms, adapt_max_ms, adapt_margin, servo_clamp_ppm
 * its running value and its reply; NULL for a reply, the default one */
struct uci_field { int cur; const char *reply; };

static void uci(const char *name, int ring, const char *r0, const char *r1, const char *r2,
                const char *r3, const char *r4)
{
	static const struct uci_field def[5] = {
		{ 12, "30\n" }, { 6, "6\n" }, { 120, "120\n" }, { 20, "20\n" }, { 700, "0\n" },
	};
	const char *r[5] = { r0, r1, r2, r3, r4 };
	uint8_t b[2048];
	size_t at = 0;
	b[at++] = (uint8_t)ring;
	for (int i = 0; i < 5; i++) {
		const char *t = r[i] ? r[i] : def[i].reply;
		size_t n = strlen(t);
		for (int k = 0; k < 4; k++)
			b[at++] = (uint8_t)((uint32_t)def[i].cur >> (8 * k));
		b[at++] = (uint8_t)n;
		memcpy(b + at, t, n);
		at += n;
	}
	put("uci", name, b, at);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: gen-corpus <directory>\n");
		return 2;
	}
	g_root = argv[1];
	dir("wire");
	dir("egress");
	dir("lock");
	dir("uci");

	/* fuzz_wire: the frames of a rig */
	wire("s0808-audio", 0, 0, 340, AUDIO, 0x1234, 0);
	wire("s0808-audio-residue", 0, 0, 342, AUDIO, 0x1235, 0);
	wire("s1608-audio", 5, 0, 628, AUDIO, 0x0001, 0);
	wire("s1608-audio-residue", 0, 0, 630, AUDIO, 0xfffe, 0);
	wire("s4000-audio", 31, 0, 1204, AUDIO, 0xbeef, 0);
	wire("s4000-audio-residue", 30, 0, 1206, AUDIO, 0x0100, 0);
	wire("downstream-40ch", 39, 1, 1492, AUDIO, 0x7f80, 0);
	wire("downstream-40ch-residue", 38, 1, 1494, AUDIO, 0x0080, 0);
	wire("s2-audio", 1, 0, 124, AUDIO, 0x0002, 0);
	wire("s1608-ctrl-cdea", 0, 0, 628, CTRL_CDEA, 0x0003, 0);
	wire("s1608-ctrl-cfea", 0, 1, 628, CTRL_CFEA, 0x0004, 0);
	wire("s1608-ctrl-00ea", 0, 0, 628, CTRL_00EA, 0x0005, 0);
	wire("downstream-ctrl-cdea", 0, 1, 1492, CTRL_CDEA, 0x0006, 0);
	wire("reac-handshake-14", 0, 0, 14, AUDIO, 0, 0);
	wire("reac-ctrl-17", 0, 0, 17, CTRL_CDEA, 0x0007, 0);
	wire("reac-18", 0, 0, 18, AUDIO, 0x0008, 0);
	wire("not-reac-ipv4", 0, 0, 60, NOT_REAC, 0, 0);
	wire("not-reac-13", 0, 0, 13, AUDIO, 0, 0);
	wire("empty-frame", 0, 0, 0, AUDIO, 0, 0);
	wire("s0808-audio-tagged", 0, 0, 340, AUDIO, 0x1236, 1);
	wire("s4000-audio-tagged", 3, 0, 1204, AUDIO, 0x4321, 1);
	wire("s1608-ctrl-tagged", 0, 0, 628, CTRL_CDEA, 0x0009, 1);
	wire("s4000-audio-not-reac", 0, 0, 1204, NOT_REAC, 0x2222, 0);
	wire("odd-width-3ch", 0, 0, 160, AUDIO, 0x000a, 0);
	wire("off-grid-200", 0, 0, 200, AUDIO, 0x000b, 0);
	wire("past-40ch-1528", 0, 0, 1528, AUDIO, 0x000c, 0);
	wire("s0808-audio-short-49", 0, 0, 49, AUDIO, 0x000d, 0);
	wire("s0808-audio-slot-0", 0, 0, 340, AUDIO, 0x000e, 0);
	wire("s0808-audio-slot-1", 1, 0, 340, AUDIO, 0x000f, 0);
	wire("s0808-audio-slot-8", 8, 0, 340, AUDIO, 0x0010, 0);
	wire("s0808-audio-slot-minus-1", -1, 0, 340, AUDIO, 0x0011, 0);
	wire("s0808-audio-slot-minus-2", -2, 0, 340, AUDIO, 0x0012, 0);
	wire("past-the-receive-buffer", 0, 0, 1700, AUDIO, 0x0013, 0);
	/* the edges of each rule: the meter's 1000 bytes, the width law's 52 and 54 (no
	 * channel), the first even width past 40, the copy reader's 50 bytes */
	wire("reac-999", 0, 0, 999, AUDIO, 0x0015, 0);
	wire("reac-1000", 0, 0, 1000, AUDIO, 0x0016, 0);
	wire("reac-52", 0, 0, 52, AUDIO, 0x0017, 0);
	wire("reac-54", 0, 0, 54, AUDIO, 0x0018, 0);
	wire("past-40ch-42ch-1564", 0, 0, 1564, AUDIO, 0x0019, 0);
	wire("reac-audio-50", 0, 0, 50, AUDIO, 0x001a, 0);
	wire("tagged-audio-50", 0, 0, 50, AUDIO, 0x001b, 1);
	{
		/* --bcast-only: a REAC frame whose destination's first byte is not 0xff,
		 * with 0xff in the byte after it */
		static uint8_t b[400];
		b[0] = (uint8_t)((2 << 1) | 1);
		size_t len = frame(b + 1, 340, AUDIO, 0x0014, 0);
		b[1] = 0x01;
		put("wire", "bcast-only-unicast-first-byte", b, len + 1);
		b[1] = 0xff;
		put("wire", "bcast-only-broadcast", b, len + 1);
	}
	{
		/* a tag with an EtherType behind it that is not REAC's */
		static uint8_t b[1300];
		b[0] = 4;
		size_t len = frame(b + 1, 60, AUDIO, 0, 1);
		b[1 + 17] = 0x18;
		put("wire", "tagged-not-reac", b, len + 1);
		/* a meter-sized frame whose tag word is 81 01, not 81 00, with REAC's
		 * EtherType four bytes on, where a tag's would be */
		len = frame(b + 1, 1204, AUDIO, 0x0102, 1);
		b[1 + 13] = 0x01;
		put("wire", "s4000-tag-81-01", b, len + 1);
	}

	/* fuzz_egress: the samples are each sign, full scale both ways, bits past 24,
	 * and three that set every byte apart */
	static const int32_t v[12] = {
		0x123456, -0x123456, 0x7fffff, -0x800000, 0x01ff8001, -1,
		0x00ff00, 0x0000ff, 0x5a5a5a, 0x00a5a5, -0x7ffffe, 0x3c3c3c,
	};
	egress("s0808-audio-ctr-1234", 0, 0x1234, v, 340, AUDIO, 0x1200);
	egress("s0808-audio-slot-3", 3, 0x10001, v, 340, AUDIO, 0x0001);
	egress("s0808-audio-slot-7", 7, 0xffff, v, 342, AUDIO, 0x0002);
	egress("s0808-audio-slot-9", 9, 0x0003, v, 340, AUDIO, 0x0003);
	egress("s0808-audio-slot-minus-1", -1, 0x0004, v, 340, AUDIO, 0x0004);
	egress("s2-audio-slot-0", 0, 0x0005, v, 124, AUDIO, 0x0005);
	egress("s1608-audio-residue-slot-15", 15, 0x0006, v, 630, AUDIO, 0x0006);
	egress("s4000-audio-slot-16", 16, 0x0007, v, 1204, AUDIO, 0x0007);
	egress("s40-downstream-slot-39", 39, 0x0008, v, 1492, AUDIO, 0x0008);
	egress("s1608-ctrl-cdea", 0, 0x0009, v, 628, CTRL_CDEA, 0x0009);
	egress("s1608-ctrl-cdea-slot-0", 0, 0x000a, v, 628, CTRL_CDEA, 0x000a);
	egress("audio-19", 0, 0x000b, v, 19, AUDIO, 0x000b);
	egress("audio-18", 0, 0x000c, v, 18, AUDIO, 0x000c);
	egress("frame-17", 0, 0x000d, v, 17, AUDIO, 0x000d);
	egress("odd-width-3ch-slot-0", 0, 0x000e, v, 160, AUDIO, 0x000e);
	egress("off-grid-200-slot-0", 0, 0x0010, v, 200, AUDIO, 0x0010);
	egress("no-frame", 0, 0x000f, v, 0, AUDIO, 0);
	put("egress", "header-cut", (const uint8_t *)"\x02\x34\x12", 3);

	/* fuzz_lock: the rate is 0 44100, 1 48000, 2 96000, 3 88200, 4 none */
	const char *saved =
		"# reac-repacer PLL warm-start lock -- per-port converged emit period.\n"
		"# <out_iface> <emit_period_ns> <rate_label_hz> <unix_epoch_saved>\n"
		"lan1 124867 96000 1749150000\n"
		"lan2 124929 96000 1749150000\n";
	locks("saved-lan1", 2, "lan1", saved);
	locks("saved-lan2", 2, "lan2", saved);
	locks("saved-lan3-absent", 2, "lan3", saved);
	locks("saved-rate-changed", 1, "lan1", saved);
	locks("rate-48000", 1, "lan1", "lan1 249000 48000\n");
	locks("rate-48000-lower-edge", 1, "lan1", "lan1 237500 48000\n");
	locks("rate-48000-upper-edge", 1, "lan1", "lan1 262500 48000\n");
	locks("rate-44100-lower-edge", 0, "lan1", "lan1 258504 44100\n");
	locks("rate-44100-past-upper-edge", 0, "lan1", "lan1 285715 44100\n");
	locks("rate-44100", 0, "lan1", "lan1 272000 44100 1\n");
	locks("rate-88200-not-detected", 3, "lan1", "lan1 136054 88200\n");
	locks("rate-none", 4, "lan1", "lan1 125000 0\n");
	locks("last-label-unterminated", 2, "lan1", "lan1 125000 9600");
	locks("rate-44100-upper-edge", 0, "lan1", "lan1 285714 44100\n");
	locks("rate-96000-lower-edge", 2, "lan1", "lan1 118750 96000\n");
	locks("rate-96000-upper-edge", 2, "lan1", "lan1 131250 96000\n");
	locks("rate-96000-below-band", 2, "lan1", "lan1 118749 96000\n");
	locks("rate-96000-other-rate-period", 2, "lan1", "lan1 250000 96000 0\n");
	locks("label-192000", 2, "lan1", "lan1 125000 192000\n");
	locks("port-prefix-lan10", 2, "lan1", "lan10 125000 96000\n");
	locks("port-15-bytes-field-longer", 2, "abcdefghijklmno", "abcdefghijklmno125000 96000 0\n");
	locks("port-15-bytes", 2, "abcdefghijklmno", "abcdefghijklmno 125000 96000 0\n");
	locks("port-hash-comment", 2, "#lan1", "#lan1 125000 96000\n");
	locks("comment-indented", 2, "lan1", "  # lan1 125000 96000\n");
	lock("nul-in-fourth-field", 2, "lan1", "lan1 125000 96000 17\0\n", 22);
	locks("period-19-digits", 2, "lan1", "lan1 0000000000000125000 96000\n");
	locks("period-18-digits", 2, "lan1", "lan1 000000000000125000 96000\n");
	locks("period-colon", 2, "lan1", "lan1 12500: 96000\n");
	locks("period-negative", 2, "lan1", "lan1 -125000 96000\n");
	locks("two-fields", 2, "lan1", "lan1 125000\n");
	locks("crlf-lines", 2, "lan1", "lan1 125000 96000\r\n");
	locks("tabs", 2, "lan1", "lan1\t125000\t96000\n");
	locks("two-records-last-wins", 2, "lan1", "lan1 124867 96000 1\nlan1 125100 96000 2\n");
	locks("empty", 2, "lan1", "");
	locks("blank-lines", 2, "lan1", "\n \n\t\n");
	put("lock", "no-port", (const uint8_t *)"\x02\x00", 2);
	{
		/* a lock of exactly 64 KiB, and of one byte more */
		static char big[65537];
		memset(big, '\n', sizeof big);
		memcpy(big, "lan1 125000 96000\n", 18);
		lock("lock-at-max", 2, "lan1", big, 65536);
		lock("lock-past-max", 2, "lan1", big, 65537);
	}

	/* fuzz_uci: NULL is the field's default reply */
	uci("defaults", 1, NULL, NULL, NULL, NULL, NULL);
	uci("defaults-ring-2048", 0, NULL, NULL, NULL, NULL, NULL);
	uci("all-absent", 1, "", "", "", "", "");
	uci("prefill-15-unterminated", 1, "15", NULL, NULL, NULL, NULL);
	uci("prefill-tab-150", 1, "\t150\n", NULL, NULL, NULL, NULL);
	uci("prefill-plus-5", 1, "+5\n", NULL, NULL, NULL, NULL);
	uci("prefill-2147483647", 1, "2147483647\n", NULL, NULL, NULL, NULL);
	uci("prefill-2147483648", 1, "2147483648\n", NULL, NULL, NULL, NULL);
	uci("prefill-4294967297", 1, "4294967297\n", NULL, NULL, NULL, NULL);
	uci("prefill-minus-2147483648", 1, "-2147483648\n", NULL, NULL, NULL, NULL);
	uci("prefill-minus-2147483649", 1, "-2147483649\n", NULL, NULL, NULL, NULL);
	uci("prefill-150000", 1, "150000\n", NULL, NULL, NULL, NULL);
	uci("prefill-150-crlf", 1, "150\r\n", NULL, NULL, NULL, NULL);
	uci("prefill-150-trailing-space", 1, "150 \n", NULL, NULL, NULL, NULL);
	uci("prefill-150-trailing-tab", 1, "150\t\n", NULL, NULL, NULL, NULL);
	uci("prefill-space-150", 1, " 150\n", NULL, NULL, NULL, NULL);
	uci("prefill-15-second-line", 1, "15\nzz\n", NULL, NULL, NULL, NULL);
	uci("prefill-15ms", 1, "15ms\n", NULL, NULL, NULL, NULL);
	uci("prefill-blank-line", 1, " \n", NULL, NULL, NULL, NULL);
	uci("prefill-sign-only", 1, "-\n", NULL, NULL, NULL, NULL);
	uci("prefill-1000", 1, "1000\n", NULL, NULL, NULL, NULL);
	uci("prefill-1001", 1, "1001\n", NULL, NULL, NULL, NULL);
	uci("prefill-0", 1, "0\n", NULL, NULL, NULL, NULL);
	uci("adapt-min-2000", 1, NULL, "2000\n", NULL, NULL, NULL);
	uci("adapt-min-1", 1, NULL, "1\n", NULL, NULL, NULL);
	uci("adapt-min-0", 1, NULL, "0\n", NULL, NULL, NULL);
	uci("adapt-max-below-min", 1, NULL, "120\n", "6\n", NULL, NULL);
	uci("adapt-max-1001", 1, NULL, NULL, "1001\n", NULL, NULL);
	uci("margin-past-ring-2048", 0, NULL, NULL, NULL, "1985\n", NULL);
	uci("margin-past-ring-16384", 1, NULL, NULL, NULL, "16321\n", NULL);
	uci("margin-minus-1", 1, NULL, NULL, NULL, "-1\n", NULL);
	uci("servo-20000", 1, NULL, NULL, NULL, NULL, "20000\n");
	uci("servo-60000", 1, NULL, NULL, NULL, NULL, "60000\n");
	uci("servo-minus-20", 1, NULL, NULL, NULL, NULL, "-20\n");
	uci("servo-minus-3", 1, NULL, NULL, NULL, NULL, "-3\n");
	put("uci", "cut-short", (const uint8_t *)"\x01\x0c\x00", 3);

	return g_fail;
}
