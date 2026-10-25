// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The warm-start lock reader, rp_lock_lookup() (src/repace_conf.c), on the
 * records the daemon writes and on two it used to misread:
 *
 * 1. A record's period is held to the standard period of the rate the daemon
 *    detected. The reader before this held it to the standard of the period's own
 *    nearest rate, so `lan1 250000 96000` (a 48 kHz period labelled 96 kHz)
 *    restored 250000 ns into a 96 kHz start.
 * 2. A record names its port by its whole first field. The reader before this
 *    took the first 15 bytes of it (sscanf "%15s"), so for the 15-byte port
 *    `abcdefghijklmno` the line `abcdefghijklmno125000 96000 0` read as that port
 *    with the period 125000.
 */
#include <stdio.h>
#include <string.h>

#include "repace_conf.h"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static long look(const char *text, const char *port, long rate)
{
	return rp_lock_lookup(text, strlen(text), port, rate);
}

int main(void)
{
	/* what lock_save writes: two comment lines and one record per port */
	const char *saved =
		"# reac-repacer PLL warm-start lock -- per-port converged emit period.\n"
		"# <out_iface> <emit_period_ns> <rate_label_hz> <unix_epoch_saved>\n"
		"lan1 124867 96000 1749150000\n"
		"lan2 124929 96000 1749150000\n";
	CHK(look(saved, "lan1", 96000) == 124867);
	CHK(look(saved, "lan2", 96000) == 124929);
	CHK(look(saved, "lan3", 96000) == 0);
	CHK(look(saved, "lan1", 48000) == 0);                 /* the rate changed since the save */
	CHK(look("lan1 249000 48000\n", "lan1", 48000) == 249000);   /* the epoch is optional */
	CHK(look("lan1 272000 44100 1\n", "lan1", 44100) == 272000);
	CHK(look("lan1 124867 96000 1\nlan1 125100 96000 2\n", "lan1", 96000) == 125100);  /* the last wins */

	/* the 5% band, bounds included: 125000 +- 6250 */
	CHK(look("lan1 118750 96000\n", "lan1", 96000) == 118750);
	CHK(look("lan1 131250 96000\n", "lan1", 96000) == 131250);
	CHK(look("lan1 118749 96000\n", "lan1", 96000) == 0);
	CHK(look("lan1 131251 96000\n", "lan1", 96000) == 0);

	/* 1: a period of another rate under this rate's label */
	CHK(look("lan1 250000 96000 0\n", "lan1", 96000) == 0);
	CHK(look("lan1 125000 48000 0\n", "lan1", 48000) == 0);

	/* 2: the port is the whole first field */
	CHK(look("abcdefghijklmno125000 96000 0\n", "abcdefghijklmno", 96000) == 0);
	CHK(look("abcdefghijklmno 125000 96000 0\n", "abcdefghijklmno", 96000) == 125000);
	CHK(look("lan10 125000 96000\n", "lan1", 96000) == 0);

	/* fields of digits only, no line with a NUL, no file past RP_LOCK_MAX */
	CHK(look("lan1 125000x 96000\n", "lan1", 96000) == 0);
	CHK(look("lan1 -125000 96000\n", "lan1", 96000) == 0);
	CHK(rp_lock_lookup("lan1 125000 96000\0\n", 19, "lan1", 96000) == 0);
	CHK(look("  # lan1 125000 96000\n", "lan1", 96000) == 0);
	CHK(look("lan1\t125000\t96000\r\n", "lan1", 96000) == 125000);
	{
		static char big[RP_LOCK_MAX + 1];
		memset(big, '\n', sizeof big);
		memcpy(big, "lan1 125000 96000\n", 18);
		CHK(rp_lock_lookup(big, RP_LOCK_MAX, "lan1", 96000) == 125000);
		CHK(rp_lock_lookup(big, RP_LOCK_MAX + 1, "lan1", 96000) == 0);
	}
	CHK(look("lan1 125000 88200\n", "lan1", 88200) == 0);  /* not a label the daemon detects */

	printf("test_conf: %d failures\n", fails);
	return fails ? 1 : 0;
}
