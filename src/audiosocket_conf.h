/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 *
 * Settings shared by every call, and the checks applied to a value before it is accepted.
 * Plain C99, no FreeSWITCH dependency: the module reads autoload_configs/audiosocket.conf.xml and
 * hands each name and value to as_config_set; a channel variable can override the result per call.
 */
#ifndef AUDIOSOCKET_CONF_H
#define AUDIOSOCKET_CONF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AS_CONF_VARS_MAX 512 /* longest list of variable names for the metadata frame */

typedef struct {
	int rate;               /* sample rate exchanged with the server */
	int connect_timeout_ms; /* TCP connect timeout */
	int jitter_ms;          /* audio held before playback starts, and again after an underrun */
	int max_queue_ms;       /* most server audio kept waiting to play */
	int send_queue_ms;      /* audio queued for a slow server before frames are dropped */
	int stall_timeout_ms;   /* end the call if the server accepts no data for this long; 0 = never */
	int dtmf;               /* forward the caller's key presses */
	char send_vars[AS_CONF_VARS_MAX]; /* comma separated channel variables sent in the metadata frame; empty = none */
} as_config_t;

#define AS_CONF_OK 0
#define AS_CONF_UNKNOWN (-1) /* no such setting */
#define AS_CONF_INVALID (-2) /* the value is not acceptable for that setting */

/* Limits of the numeric settings. The same limits apply to the channel variables that override them. */
#define AS_CONNECT_TIMEOUT_MIN 1
#define AS_CONNECT_TIMEOUT_MAX 60000
#define AS_JITTER_MS_MIN 20
#define AS_JITTER_MS_MAX 10000
#define AS_MAX_QUEUE_MS_MIN 100
#define AS_MAX_QUEUE_MS_MAX 60000 /* 60 s of 48 kHz audio is about 5.8 MB per call, so keep it modest */
#define AS_SEND_QUEUE_MS_MIN 100
#define AS_SEND_QUEUE_MS_MAX 10000
#define AS_STALL_TIMEOUT_MS_MIN 0
#define AS_STALL_TIMEOUT_MS_MAX 600000

/* Built-in defaults: 8000 Hz, connect 2000 ms, jitter 200 ms, max queue 10000 ms, send queue 500 ms,
 * stall timeout 10000 ms, key presses on, no metadata. */
void as_config_defaults(as_config_t *c);

/* Apply one setting by its name in the config file: default-rate, connect-timeout-ms, jitter-buffer-ms,
 * max-queue-ms, send-queue-ms, stall-timeout-ms, dtmf, send-vars. The value is checked and c is left
 * unchanged on error. Returns AS_CONF_OK, AS_CONF_UNKNOWN or AS_CONF_INVALID. */
int as_config_set(as_config_t *c, const char *name, const char *value);

/* true, yes, on, 1 give 1; false, no, off, 0 give 0 (any case). Returns 0 on success, -1 otherwise. */
int as_parse_bool(const char *s, int *out);

/* A whole decimal number within [min, max]; surrounding spaces are allowed. Returns 0 on success, -1 otherwise. */
int as_parse_int(const char *s, int min, int max, int *out);

/* A list of variable names for the metadata frame: letters, digits, '_', '.', '-', ',' and spaces only.
 * Returns 1 if acceptable (an empty list is), 0 otherwise. */
int as_send_vars_valid(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* AUDIOSOCKET_CONF_H */
