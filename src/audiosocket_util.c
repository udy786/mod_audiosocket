/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 */
#define _POSIX_C_SOURCE 200809L
#include "audiosocket_util.h"

#include <errno.h>
#include <time.h>

long long as_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

void as_pacer_init(as_pacer_t *p, long long step_us, int max_burst)
{
	p->step_us = step_us > 0 ? step_us : 20000;
	p->owed_us = 0;
	p->last_us = 0;
	p->max_burst = max_burst > 0 ? max_burst : 1;
	p->started = 0;
}

int as_pacer_due(as_pacer_t *p, long long now_us)
{
	long long half = p->step_us / 2;
	long long limit = (long long)p->max_burst * p->step_us;
	long long n;

	if (!p->started) {
		p->started = 1;
		p->last_us = now_us;
		return 1;
	}
	if (now_us > p->last_us) {
		p->owed_us += now_us - p->last_us;
	}
	p->last_us = now_us; /* a clock that stepped back just costs a tick */
	if (p->owed_us > limit) {
		p->owed_us = limit; /* do not try to make up a long stall */
	}
	if (p->owed_us + half < 0) {
		return 0;
	}
	n = (p->owed_us + half) / p->step_us;
	if (n > p->max_burst) {
		n = p->max_burst;
	}
	p->owed_us -= n * p->step_us;
	return (int)n;
}

void as_stall_init(as_stall_t *s, long long timeout_us)
{
	s->timeout_us = timeout_us;
	s->since_us = 0;
	s->last_sent = 0;
	s->armed = 0;
}

int as_stall_check(as_stall_t *s, long long now_us, size_t queued_bytes, unsigned long long sent_total)
{
	if (s->timeout_us <= 0) {
		return 0;
	}
	if (queued_bytes == 0 || sent_total != s->last_sent) {
		s->last_sent = sent_total;
		s->armed = 0;
		return 0;
	}
	if (!s->armed) {
		s->armed = 1;
		s->since_us = now_us;
		return 0;
	}
	return now_us - s->since_us >= s->timeout_us;
}

size_t as_downmix_mono(const int16_t *in, size_t frames, int channels, int16_t *out)
{
	size_t i;
	int c;

	if (channels <= 1) {
		if (in != out) {
			for (i = 0; i < frames; i++) {
				out[i] = in[i];
			}
		}
		return frames;
	}
	for (i = 0; i < frames; i++) { /* writing out[i] never overwrites an input not yet read, even in place */
		long sum = 0;

		for (c = 0; c < channels; c++) {
			sum += in[i * (size_t)channels + (size_t)c];
		}
		out[i] = (int16_t)(sum / channels);
	}
	return frames;
}

void as_sanitize_text(char *s)
{
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;

		if (ch < 0x20 || ch == 0x7f) {
			*s = '?';
		}
	}
}

int as_errno_is_disconnect(int e)
{
	switch (e) {
	case ECONNRESET:
	case EPIPE:
	case ETIMEDOUT:
	case ECONNABORTED:
	case EHOSTUNREACH:
	case ENETUNREACH:
	case ENETDOWN:
#ifdef EHOSTDOWN
	case EHOSTDOWN:
#endif
	case ENOTCONN:
		return 1;
	default:
		return 0;
	}
}
