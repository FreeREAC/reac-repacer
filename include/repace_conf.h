// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* repace_conf -- the texts reac-repacer reads besides its command line: the
 * warm-start lock it wrote on an earlier run. Every function here is pure: the
 * daemon opens the file and hands the bytes in. */
#ifndef REPACE_CONF_H
#define REPACE_CONF_H

#include <stddef.h>

/* The warm-start lock, the file's n bytes. The daemon writes it, one record per
 * output port:
 *   <out_iface> <emit_period_ns> <rate_label_hz> [<unix_epoch_saved>]
 * Lines end at LF. Fields are separated by spaces, tabs or CRs; a line whose first
 * field starts with '#', or that has no field, is a comment. A record is a line
 * whose first field is the port name (at most 15 bytes) and whose second and third
 * fields are decimal digits only (at most 18 each); a fourth field and any after
 * it are not read. A line holding a NUL byte is no record, and a text of more than
 * RP_LOCK_MAX bytes is no lock the daemon wrote.
 *
 * Returns the period of the last record for `port` whose rate label is `rate_hz`
 * and whose period is within 5% of the standard period of that rate: the
 * nanoseconds of one 12-sample frame, 1e9 * 12 / rate_hz rounded to the nearest
 * integer, the 5% being that integer divided by 20 (integer division), bounds
 * included. rate_hz is one of 44100, 48000 or 96000; for any other, and when no
 * record qualifies, 0. */
#define RP_LOCK_MAX 65536
long rp_lock_lookup(const char *text, size_t n, const char *port, long rate_hz);

#endif /* REPACE_CONF_H */
