/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 *
 * Audio buffers. Plain C99 plus GCC/Clang __atomic builtins; no FreeSWITCH dependency.
 *
 * as_ring_t   single-producer single-consumer byte ring buffer, lock-free.
 *             One thread may call the write functions and one other thread the read functions.
 *             as_ring_level and as_ring_space may be called from any thread for monitoring.
 *             It never blocks.
 * as_jitter_t fixed-size-frame playout buffer for audio coming from the server.
 *             as_jitter_put and as_jitter_get are called from the same thread (the call's own thread);
 *             only the counters and as_jitter_pending may be read from another thread.
 *             Playout starts once a small prefill is queued, or once audio shorter than the prefill has
 *             stopped arriving for a few ticks, so a short reply is never held back for ever. A last
 *             partial frame is padded with silence. It returns silence on underrun and trims latency when
 *             too much is queued. In draining mode (the server has gone away) everything queued is played
 *             at once, without waiting for a prefill.
 */
#ifndef AUDIOSOCKET_BUF_H
#define AUDIOSOCKET_BUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t *data;
	size_t cap;  /* power of two */
	size_t mask; /* cap - 1 */
	size_t head; /* total bytes ever written (producer owns) */
	size_t tail; /* total bytes ever read (consumer owns) */
} as_ring_t;

/* Allocate a ring of at least min_cap bytes (rounded up to a power of two).
 * Returns 0 on success, -1 on failure. */
int as_ring_init(as_ring_t *r, size_t min_cap);
void as_ring_free(as_ring_t *r);

/* Producer side. Writes up to n bytes, returns how many fit. Never blocks. */
size_t as_ring_write(as_ring_t *r, const uint8_t *src, size_t n);
/* Consumer side. Reads up to n bytes, returns how many were available. Never blocks. */
size_t as_ring_read(as_ring_t *r, uint8_t *dst, size_t n);
/* Consumer side. Copies up to n bytes without consuming them, returns how many were copied.
 * Used to send to a socket: peek, send, then skip as many bytes as the socket accepted. */
size_t as_ring_peek(const as_ring_t *r, uint8_t *dst, size_t n);
/* Consumer side. Drops up to n of the oldest bytes, returns how many were dropped. */
size_t as_ring_skip(as_ring_t *r, size_t n);
/* Consumer view of how many bytes are readable now. */
size_t as_ring_level(const as_ring_t *r);
/* Producer view of how many bytes can be written now. */
size_t as_ring_space(const as_ring_t *r);

typedef struct {
	as_ring_t ring;
	size_t frame_bytes;   /* bytes per playout tick, e.g. 320 for 20 ms of 8 kHz mono slin */
	size_t prefill_bytes; /* queued before playout starts, and again after an underrun */
	size_t max_bytes;     /* above this the oldest audio is dropped (only the surplus) */
	int playing;
	int draining;         /* the source has gone: play out what is queued, wait for nothing */
	unsigned idle_ticks;  /* ticks spent waiting with less than the prefill queued and nothing new arriving */
	unsigned flush_ticks; /* after this many such ticks the short audio is played anyway */
	/* counters, written with relaxed atomics so another thread can read them */
	unsigned long underruns;          /* playback ran dry while the source may still be talking */
	unsigned long trimmed_bytes;
	unsigned long dropped_full_bytes;
} as_jitter_t;

#define AS_JITTER_FLUSH_TICKS 5 /* 100 ms at 20 ms per tick */

/* frame_bytes must be even and > 0, 1 <= prefill_frames < max_frames.
 * Returns 0 on success, -1 on bad arguments or allocation failure. */
int as_jitter_init(as_jitter_t *j, size_t frame_bytes, unsigned prefill_frames, unsigned max_frames);
void as_jitter_free(as_jitter_t *j);

/* Queue received audio. Only whole 16 bit samples are kept (n is rounded down to even).
 * Returns the number of bytes queued; the rest is counted in dropped_full_bytes. */
size_t as_jitter_put(as_jitter_t *j, const uint8_t *data, size_t n);

/* Once per tick. Fill out with exactly frame_bytes. Returns 1 if it holds queued audio (a final partial
 * frame is padded with silence), 0 if it was filled with silence (nothing queued, still waiting for the
 * prefill, or an underrun). */
int as_jitter_get(as_jitter_t *j, uint8_t *out);

/* The source has gone away: from now on play out everything queued without waiting, and do not count
 * running dry as an underrun. */
void as_jitter_drain(as_jitter_t *j);

/* Bytes queued and not yet played. May be called from any thread for monitoring. */
size_t as_jitter_pending(const as_jitter_t *j);

/* Drop everything queued and prefill again. */
void as_jitter_flush(as_jitter_t *j);

#ifdef __cplusplus
}
#endif

#endif /* AUDIOSOCKET_BUF_H */
