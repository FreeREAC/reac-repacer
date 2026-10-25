// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* repace_conf -- the warm-start lock reader and the hot-parameter ranges of
 * reac-repacer (include/repace_conf.h). */
#include <string.h>

#include "repace_conf.h"

void rp_hot_clamp(struct rp_hot *p, int margin_max)
{
	if (p->prefill_ms < RP_PREFILL_MS_MIN)
		p->prefill_ms = RP_PREFILL_MS_MIN;
	if (p->prefill_ms > RP_PREFILL_MS_MAX)
		p->prefill_ms = RP_PREFILL_MS_MAX;
	if (p->adapt_min_ms < RP_ADAPT_MS_MIN)
		p->adapt_min_ms = RP_ADAPT_MS_MIN;
	if (p->adapt_min_ms > RP_ADAPT_MS_MAX)
		p->adapt_min_ms = RP_ADAPT_MS_MAX;
	if (p->adapt_max_ms > RP_ADAPT_MS_MAX)
		p->adapt_max_ms = RP_ADAPT_MS_MAX;
	if (p->adapt_max_ms < p->adapt_min_ms)
		p->adapt_max_ms = p->adapt_min_ms;
	if (p->adapt_margin < 0)
		p->adapt_margin = 0;
	if (p->adapt_margin > margin_max)
		p->adapt_margin = margin_max;
	if (p->servo_clamp_ppm < 0)
		p->servo_clamp_ppm = 0;
	if (p->servo_clamp_ppm > RP_SERVO_CLAMP_PPM_MAX)
		p->servo_clamp_ppm = RP_SERVO_CLAMP_PPM_MAX;
}

/* the standard period (ns) of one 12-sample frame at a rate label, 0 for none */
static long std_period(long rate_hz)
{
	switch (rate_hz) {
	case 44100: return 272109;   /* 3675 frames/s */
	case 48000: return 250000;   /* 4000 frames/s */
	case 96000: return 125000;   /* 8000 frames/s */
	}
	return 0;
}

static int is_sep(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

/* the next field of [*p, end): its start and length, 0 when the line has no more */
static size_t field(const char **p, const char *end, const char **at)
{
	const char *s = *p;
	while (s < end && is_sep(*s))
		s++;
	const char *e = s;
	while (e < end && !is_sep(*e))
		e++;
	*at = s;
	*p = e;
	return (size_t)(e - s);
}

/* a field of decimal digits only, at most 18 of them; -1 for any other */
static long long digits(const char *s, size_t len)
{
	if (len == 0 || len > 18)
		return -1;
	long long v = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (s[i] - '0');
	}
	return v;
}

long rp_lock_lookup(const char *text, size_t n, const char *port, long rate_hz)
{
	long std = std_period(rate_hz);
	if (std == 0 || n > RP_LOCK_MAX)
		return 0;
	size_t plen = strlen(port);
	long found = 0;
	const char *line = text, *stop = text + n;
	while (line < stop) {
		const char *lf = memchr(line, '\n', (size_t)(stop - line));
		const char *end = lf ? lf : stop;
		const char *p = line, *name, *per_at, *lab_at;
		int has_nul = memchr(line, '\0', (size_t)(end - line)) != NULL;
		size_t name_len = field(&p, end, &name);
		size_t per_len = field(&p, end, &per_at);
		size_t lab_len = field(&p, end, &lab_at);
		line = lf ? lf + 1 : stop;
		if (has_nul)
			continue;
		if (name_len == 0 || name[0] == '#')
			continue;
		if (name_len > 15 || name_len != plen || memcmp(name, port, plen) != 0)
			continue;
		long long per = digits(per_at, per_len), lab = digits(lab_at, lab_len);
		if (per < 0 || lab != rate_hz)
			continue;
		if (per < std - std / 20 || per > std + std / 20)
			continue;
		found = (long)per;                 /* the last matching record wins (newest appended) */
	}
	return found;
}
