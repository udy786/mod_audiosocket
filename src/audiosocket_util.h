/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 *
 * Small helpers that the module needs and that can be tested without FreeSWITCH: a monotonic clock, the
 * pacer that decides how many playback frames are due, the watchdog for a server that stopped reading,
 * a stereo-to-mono downmix, text cleaning for logs and listings, and a classification of socket errors.
 * Plain C99.
 */
#ifndef AUDIOSOCKET_UTIL_H
#define AUDIOSOCKET_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Microseconds on a monotonic clock (not affected by the system time being set). */
long long as_now_us(void);

/* The pacer turns elapsed time into a number of playback frames to write.
 * The loop that drives playback is woken by the call's incoming audio, which normally arrives once per
 * packet, but FreeSWITCH can also return at once without waiting (for example while the call's codec is
 * being changed) and the read and write packet times can differ. Counting returns would then play audio
 * too fast or too slow; counting time does not. The average rate is exact; each answer is rounded to the
 * nearest whole frame. A stall longer than max_burst frames is not made up for. */
typedef struct {
	long long step_us;   /* length of one playback frame */
	long long owed_us;   /* time passed that has not been paid out in frames (can be slightly negative) */
	long long last_us;
	int max_burst;
	int started;
} as_pacer_t;

void as_pacer_init(as_pacer_t *p, long long step_us, int max_burst);

/* Frames to write now, 0 to max_burst. The first call returns 1. */
int as_pacer_due(as_pacer_t *p, long long now_us);

/* Watchdog for a server that has stopped reading: if bytes are waiting to be sent and not one more byte
 * has been accepted for timeout_us, the server is stalled. timeout_us of 0 disables it. */
typedef struct {
	long long timeout_us;
	long long since_us;
	unsigned long long last_sent;
	int armed;
} as_stall_t;

void as_stall_init(as_stall_t *s, long long timeout_us);

/* queued_bytes: waiting to be sent. sent_total: bytes accepted by the socket so far. Returns 1 once stalled. */
int as_stall_check(as_stall_t *s, long long now_us, size_t queued_bytes, unsigned long long sent_total);

/* Average interleaved 16 bit samples down to one channel. in and out may be the same buffer.
 * frames is the number of sample frames (samples per channel). Returns frames. */
size_t as_downmix_mono(const int16_t *in, size_t frames, int channels, int16_t *out);

/* Replace control characters (below 0x20, and 0x7f) by '?', so text from a dialplan cannot disturb a
 * log or a terminal. */
void as_sanitize_text(char *s);

/* 1 if errno value e means the connection to the server is gone (reset, broken pipe, timed out,
 * unreachable); 0 for other errors. */
int as_errno_is_disconnect(int e);

#ifdef __cplusplus
}
#endif

#endif /* AUDIOSOCKET_UTIL_H */
