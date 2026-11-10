/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 */
#include "audiosocket_buf.h"

#include <stdlib.h>
#include <string.h>

#if !defined(__GNUC__)
#error "audiosocket_buf.c needs GCC or Clang atomic builtins"
#endif

#define LOAD_ACQ(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define LOAD_RLX(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

int as_ring_init(as_ring_t *r, size_t min_cap)
{
	size_t cap = 2;

	memset(r, 0, sizeof(*r));
	if (min_cap > ((size_t)1 << (sizeof(size_t) * 8 - 2))) {
		return -1;
	}
	while (cap < min_cap) {
		cap <<= 1;
	}
	r->data = (uint8_t *)malloc(cap);
	if (!r->data) {
		return -1;
	}
	r->cap = cap;
	r->mask = cap - 1;
	return 0;
}

void as_ring_free(as_ring_t *r)
{
	free(r->data);
	memset(r, 0, sizeof(*r));
}

size_t as_ring_level(const as_ring_t *r)
{
	/* A consistent snapshot of head minus tail, from any thread. Reading the two indices one after the
	 * other is not enough when the owners are moving them: with the tail read first the difference can
	 * come out larger than the ring, with the head first it can wrap below zero. So the tail is read
	 * again after the head, and the read is repeated until the tail did not move in between. While the
	 * tail stood still the head cannot be more than one ring ahead of it, so the difference is exact.
	 * Under constant movement the retries are limited and the result is clamped to 0..cap. */
	size_t t, t2, h, d;
	int i;

	t2 = LOAD_ACQ(&r->tail);
	h = LOAD_ACQ(&r->head);
	for (i = 0; i < 8; i++) {
		t = t2;
		h = LOAD_ACQ(&r->head);
		t2 = LOAD_ACQ(&r->tail);
		if (t == t2) {
			break;
		}
	}
	d = h - t2;
	if (d > r->cap) {
		/* the tail moved past the head we read, or the head ran ahead: clamp */
		d = (d > ((size_t)-1) / 2) ? 0 : r->cap;
	}
	return d;
}

size_t as_ring_space(const as_ring_t *r)
{
	return r->cap - as_ring_level(r);
}

size_t as_ring_write(as_ring_t *r, const uint8_t *src, size_t n)
{
	size_t head = LOAD_RLX(&r->head);
	size_t tail = LOAD_ACQ(&r->tail);
	size_t space = r->cap - (head - tail);
	size_t idx, first;

	if (n > space) {
		n = space;
	}
	if (n == 0) {
		return 0;
	}
	idx = head & r->mask;
	first = r->cap - idx;
	if (first > n) {
		first = n;
	}
	memcpy(r->data + idx, src, first);
	if (n > first) {
		memcpy(r->data, src + first, n - first);
	}
	STORE_REL(&r->head, head + n);
	return n;
}

size_t as_ring_read(as_ring_t *r, uint8_t *dst, size_t n)
{
	size_t tail = LOAD_RLX(&r->tail);
	size_t head = LOAD_ACQ(&r->head);
	size_t avail = head - tail;
	size_t idx, first;

	if (n > avail) {
		n = avail;
	}
	if (n == 0) {
		return 0;
	}
	idx = tail & r->mask;
	first = r->cap - idx;
	if (first > n) {
		first = n;
	}
	memcpy(dst, r->data + idx, first);
	if (n > first) {
		memcpy(dst + first, r->data, n - first);
	}
	STORE_REL(&r->tail, tail + n);
	return n;
}

size_t as_ring_peek(const as_ring_t *r, uint8_t *dst, size_t n)
{
	size_t tail = LOAD_RLX(&r->tail);
	size_t head = LOAD_ACQ(&r->head);
	size_t avail = head - tail;
	size_t idx, first;

	if (n > avail) {
		n = avail;
	}
	if (n == 0) {
		return 0;
	}
	idx = tail & r->mask;
	first = r->cap - idx;
	if (first > n) {
		first = n;
	}
	memcpy(dst, r->data + idx, first);
	if (n > first) {
		memcpy(dst + first, r->data, n - first);
	}
	return n;
}

size_t as_ring_skip(as_ring_t *r, size_t n)
{
	size_t tail = LOAD_RLX(&r->tail);
	size_t avail = LOAD_ACQ(&r->head) - tail;

	if (n > avail) {
		n = avail;
	}
	if (n) {
		STORE_REL(&r->tail, tail + n);
	}
	return n;
}

int as_jitter_init(as_jitter_t *j, size_t frame_bytes, unsigned prefill_frames, unsigned max_frames)
{
	memset(j, 0, sizeof(*j));
	if (frame_bytes == 0 || (frame_bytes & 1) || prefill_frames < 1 || max_frames <= prefill_frames) {
		return -1;
	}
	j->frame_bytes = frame_bytes;
	j->prefill_bytes = frame_bytes * prefill_frames;
	j->max_bytes = frame_bytes * max_frames;
	j->flush_ticks = AS_JITTER_FLUSH_TICKS;
	/* room for the maximum level plus one more burst of the same size */
	return as_ring_init(&j->ring, j->max_bytes * 2);
}

void as_jitter_free(as_jitter_t *j)
{
	as_ring_free(&j->ring);
	memset(j, 0, sizeof(*j));
}

size_t as_jitter_put(as_jitter_t *j, const uint8_t *data, size_t n)
{
	size_t want = n & ~(size_t)1; /* whole 16 bit samples only */
	size_t room = as_ring_space(&j->ring) & ~(size_t)1;
	size_t put;

	if (want > room) {
		want = room;
	}
	put = want ? as_ring_write(&j->ring, data, want) : 0;
	if (put < n) {
		__atomic_fetch_add(&j->dropped_full_bytes, (unsigned long)(n - put), __ATOMIC_RELAXED);
	}
	if (put) {
		j->idle_ticks = 0; /* more audio is arriving: keep waiting for the prefill */
	}
	return put;
}

int as_jitter_get(as_jitter_t *j, uint8_t *out)
{
	size_t level = as_ring_level(&j->ring);

	/* more than the allowed maximum queued: drop only the surplus, oldest first, in whole frames.
	 * The maximum is large on purpose, because a bot may send speech faster than real time. */
	if (level > j->max_bytes) {
		size_t drop = ((level - j->max_bytes + j->frame_bytes - 1) / j->frame_bytes) * j->frame_bytes;
		drop = as_ring_skip(&j->ring, drop);
		__atomic_fetch_add(&j->trimmed_bytes, (unsigned long)drop, __ATOMIC_RELAXED);
		level -= drop;
	}

	if (!j->playing) {
		if (level >= j->prefill_bytes) {
			j->playing = 1;
		} else if (level > 0) {
			/* Less than the prefill is queued. Normally more is on its way, so wait; but audio that is
			 * shorter than the prefill (a short reply) would wait for ever, so once nothing new has
			 * arrived for a few ticks, or the source has gone, play what there is. */
			if (j->draining || j->idle_ticks >= j->flush_ticks) {
				j->playing = 1;
			} else {
				j->idle_ticks++;
			}
		}
	}
	if (j->playing) {
		if (level >= j->frame_bytes) {
			as_ring_read(&j->ring, out, j->frame_bytes);
			j->idle_ticks = 0;
			return 1;
		}
		if (level > 0) {
			/* Only a partial frame is left. The rest of it may be on its way, so wait a few ticks (playing
			 * silence); if nothing arrives, or the source has gone, play it padded with silence. This is
			 * the tail of the audio, not an underrun, and a new reply prefills again. */
			if (j->draining || j->idle_ticks >= j->flush_ticks) {
				as_ring_read(&j->ring, out, level);
				memset(out + level, 0, j->frame_bytes - level);
				j->playing = 0;
				j->idle_ticks = 0;
				return 1;
			}
			j->idle_ticks++;
			memset(out, 0, j->frame_bytes);
			return 0;
		}
		/* nothing left while playing: an underrun, unless the source is gone and this is simply the end */
		if (!j->draining) {
			__atomic_fetch_add(&j->underruns, 1UL, __ATOMIC_RELAXED);
		}
		j->playing = 0;
		j->idle_ticks = 0;
	}
	memset(out, 0, j->frame_bytes);
	return 0;
}

void as_jitter_drain(as_jitter_t *j)
{
	j->draining = 1;
}

size_t as_jitter_pending(const as_jitter_t *j)
{
	return as_ring_level(&j->ring);
}

void as_jitter_flush(as_jitter_t *j)
{
	as_ring_skip(&j->ring, as_ring_level(&j->ring));
	j->playing = 0;
	j->idle_ticks = 0;
}
