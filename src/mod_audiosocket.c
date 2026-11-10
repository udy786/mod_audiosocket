/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 *
 * mod_audiosocket: stream call audio to an AudioSocket server and play the
 * server's audio back to the caller (full duplex).
 *
 * How a call runs. The application's own thread loops, woken by the call's incoming packets:
 *   - reading the call's frame drives the media; a read-replace media bug sees each frame of the
 *     caller's audio (always inside that read, in this same thread) and queues it, framed, for the server;
 *   - the server's audio frames are resampled to the call's rate and queued in a jitter buffer; playback
 *     frames are written to the call according to elapsed time, not to the number of loop passes, because
 *     the read can return at once without waiting;
 *   - if the server hangs up or goes away, the audio it already sent is still played out before the call
 *     moves on, and a server that has stopped accepting data is given up on after a timeout.
 * No extra threads are created. All socket calls on the media path are non-blocking.
 *
 * Dialplan:  audiosocket <host:port> <uuid> [rate]
 * Settings come from, in order of priority: the app's rate argument, channel variables, the optional
 * autoload_configs/audiosocket.conf.xml, and built-in defaults (see audiosocket_conf.c). Channel variables read:
 *   audiosocket_rate (8000), audiosocket_connect_timeout_ms (2000), audiosocket_jitter_ms (200),
 *   audiosocket_max_queue_ms (10000), audiosocket_stall_timeout_ms (10000, 0 = never), audiosocket_dtmf (on),
 *   audiosocket_send_vars (none). Numbers outside their allowed range are refused with a warning.
 * audiosocket_send_vars names channel variables to send once, as JSON, in a metadata frame (kind 0x40, an
 * extension to the Asterisk protocol) right after the UUID frame.
 * Channel variables set when the application ends:
 *   audiosocket_status          ok, server_hangup, server_closed, server_stalled, connect_failed, bad_args, error
 *   audiosocket_frames_sent     audio frames sent to the server
 *   audiosocket_frames_dropped  audio frames dropped because the server was too slow
 *   audiosocket_frames_received audio frames received from the server
 *   audiosocket_underruns       times playback ran dry
 *   audiosocket_dtmf_sent       key presses sent to the server
 */

#include <switch.h>

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audiosocket_buf.h"
#include "audiosocket_conf.h"
#include "audiosocket_json.h"
#include "audiosocket_net.h"
#include "audiosocket_proto.h"
#include "audiosocket_util.h"

#define AUDIOSOCKET_VERSION "0.1.0-dev"

SWITCH_MODULE_LOAD_FUNCTION(mod_audiosocket_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audiosocket_shutdown);
SWITCH_MODULE_DEFINITION(mod_audiosocket, mod_audiosocket_load, mod_audiosocket_shutdown, NULL);

#define AUDIOSOCKET_APP_NAME "audiosocket"
#define AUDIOSOCKET_APP_SYNOPSIS "Stream call audio to an AudioSocket server"
#define AUDIOSOCKET_APP_DESC \
	"Connects the call's audio to an AudioSocket server over TCP, both directions at once.\n" \
	"Usage: audiosocket <host:port> <uuid> [rate]\n" \
	"Channel variables (each overrides audiosocket.conf.xml): audiosocket_rate (default 8000),\n" \
	"audiosocket_connect_timeout_ms (2000), audiosocket_jitter_ms (200), audiosocket_max_queue_ms (10000),\n" \
	"audiosocket_stall_timeout_ms (10000, 0 = never), audiosocket_dtmf (on), audiosocket_send_vars (none).\n" \
	"Run it after answer. Sets audiosocket_status, audiosocket_frames_sent, audiosocket_frames_dropped,\n" \
	"audiosocket_frames_received, audiosocket_underruns and audiosocket_dtmf_sent when it ends."

#define SEND_TIMEOUT_MS 1000
#define METADATA_MAX 8192            /* largest metadata frame payload */
#define RESAMPLE_OUT_SAMPLES 8192    /* output buffer of each resampler, in samples */
#define RATE_SLOTS 10                /* the nine protocol rates, plus one slot for any other rate */
#define RECV_ROUNDS_PER_TICK 8       /* socket reads per tick, 4096 bytes each */
#define MAX_FRAMES_PER_TICK 400      /* frames taken from the server in one tick; the rest waits in the socket */
#define MAX_PACE_BURST 5             /* most playback frames written at once to catch up after a stall */
#define PLAYBACK_CHECK_TICKS 50      /* how often the call's write codec is checked for a change */
#define DRAIN_EXTRA_TICKS 3          /* silent ticks after the last queued audio, so it is sent before the call moves on */
#define PLAY_BUF_BYTES SWITCH_RECOMMENDED_BUFFER_SIZE

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* Call counters are written by the call's own thread and read by the stats command from another
 * thread, so they are accessed with relaxed atomics. */
#define CADD(p, n) __atomic_fetch_add((p), (n), __ATOMIC_RELAXED)
#define CGET(p) __atomic_load_n((p), __ATOMIC_RELAXED)

/* A resampler for each source rate, made when first needed and kept for the call. Without the cache a
 * server alternating between rates would force one to be built and destroyed for every frame. */
typedef struct {
	switch_audio_resampler_t *rs[RATE_SLOTS];
	int other_rate; /* the rate held in the last slot, which is shared by all rates the protocol does not list */
} as_rs_cache_t;

/* Everything one call needs. Lives on the application's stack for the length of the call. */
typedef struct as_call {
	switch_core_session_t *session;
	int fd;
	/* caller to server */
	int rate;      /* sample rate sent to the server */
	int kind;      /* AudioSocket audio kind for that rate */
	as_ring_t out; /* bytes waiting for the socket, whole frames with their headers */
	as_rs_cache_t out_rs;
	int broken;    /* the socket failed while sending */
	unsigned long frames_sent;
	unsigned long frames_dropped;
	unsigned long long bytes_sent;
	unsigned long dtmf_sent;  /* key presses forwarded to the server */
	int dtmf_enabled;         /* forward key presses (audiosocket_dtmf, default on) */
	unsigned long bug_callbacks;
	unsigned long bug_reads;
	unsigned long long bug_read_bytes;
	/* server to caller */
	int jitter_ms, max_queue_ms; /* settings, kept to set playback up again if the call's codec changes */
	int write_rate;       /* sample rate of the call's write side */
	int write_channels;
	size_t frame_bytes;   /* bytes of 16 bit mono audio per packet on the write side */
	long long write_us;   /* length of one packet on the write side */
	int playback;         /* 1 when the return path is active */
	int codec_ready;
	switch_codec_t wcodec;
	as_jitter_t jitter;
	unsigned long underruns_acc; /* underruns of earlier playback set-ups of this call */
	as_rs_cache_t in_rs;
	unsigned long frames_in;
	unsigned long long bytes_in;
	/* registry of active calls, for audiosocket_stats */
	struct as_call *prev, *next;
	char uuid[40];
	char peer[300];
	switch_time_t started;
} as_call_t;

typedef struct {
	unsigned long long calls_started, frames_sent, frames_dropped, bytes_sent, frames_in, underruns, dtmf;
	unsigned long long ended_ok, ended_server_hangup, ended_server_closed, ended_server_stalled;
	unsigned long long ended_error, ended_bad_args, ended_connect_failed;
} as_totals_t;

/* Module-wide state. Finished calls are added to the totals; active calls are read live. */
static struct {
	switch_mutex_t *mutex;
	as_config_t config; /* settings from audiosocket.conf.xml; read and replaced under the mutex */
	as_call_t *head;    /* active calls */
	switch_time_t since;
	as_totals_t t;
} g;

static void stats_lock(void)
{
	if (g.mutex) {
		switch_mutex_lock(g.mutex);
	}
}

static void stats_unlock(void)
{
	if (g.mutex) {
		switch_mutex_unlock(g.mutex);
	}
}

static void stats_call_started(void)
{
	stats_lock();
	g.t.calls_started++;
	stats_unlock();
}

static unsigned long call_underruns(const as_call_t *c)
{
	return c->underruns_acc + CGET(&c->jitter.underruns);
}

static void register_call(as_call_t *c, const char *uuid, const char *peer)
{
	snprintf(c->uuid, sizeof(c->uuid), "%s", uuid);
	snprintf(c->peer, sizeof(c->peer), "%s", peer);
	as_sanitize_text(c->uuid); /* these come from the dialplan and are printed by the stats command */
	as_sanitize_text(c->peer);
	c->started = switch_micro_time_now();
	stats_lock();
	c->prev = NULL;
	c->next = g.head;
	if (g.head) {
		g.head->prev = c;
	}
	g.head = c;
	stats_unlock();
}

static void unregister_call(as_call_t *c)
{
	stats_lock();
	if (c->prev) {
		c->prev->next = c->next;
	} else if (g.head == c) {
		g.head = c->next;
	}
	if (c->next) {
		c->next->prev = c->prev;
	}
	c->next = c->prev = NULL;
	stats_unlock();
}

static void stats_add_totals(const as_call_t *c)
{
	stats_lock();
	g.t.frames_sent += CGET(&c->frames_sent);
	g.t.frames_dropped += CGET(&c->frames_dropped);
	g.t.bytes_sent += CGET(&c->bytes_sent);
	g.t.frames_in += CGET(&c->frames_in);
	g.t.underruns += call_underruns(c);
	g.t.dtmf += CGET(&c->dtmf_sent);
	stats_unlock();
}

/* Sets the channel variable and counts how the call ended. Called once on every exit path. */
static void set_status(switch_channel_t *channel, const char *status)
{
	switch_channel_set_variable(channel, "audiosocket_status", status);
	stats_lock();
	if (!strcmp(status, "ok")) {
		g.t.ended_ok++;
	} else if (!strcmp(status, "server_hangup")) {
		g.t.ended_server_hangup++;
	} else if (!strcmp(status, "server_closed")) {
		g.t.ended_server_closed++;
	} else if (!strcmp(status, "server_stalled")) {
		g.t.ended_server_stalled++;
	} else if (!strcmp(status, "connect_failed")) {
		g.t.ended_connect_failed++;
	} else if (!strcmp(status, "bad_args")) {
		g.t.ended_bad_args++;
	} else {
		g.t.ended_error++;
	}
	stats_unlock();
}

/* ------------------------------------------------------------------------------------------
 * Resampling
 * ---------------------------------------------------------------------------------------- */

static int rs_slot(int in_rate)
{
	int k = as_rate_kind(in_rate);

	return k >= 0 ? k - AS_KIND_AUDIO_8K : RATE_SLOTS - 1;
}

static void rs_free_all(as_rs_cache_t *cache)
{
	int i;

	for (i = 0; i < RATE_SLOTS; i++) {
		if (cache->rs[i]) {
			switch_resample_destroy(&cache->rs[i]);
		}
	}
	cache->other_rate = 0;
}

/* The resampler from in_rate to out_rate. out_rate must be the same for every use of one cache. */
static switch_audio_resampler_t *rs_get(as_rs_cache_t *cache, int in_rate, int out_rate)
{
	int slot = rs_slot(in_rate);

	if (slot == RATE_SLOTS - 1 && cache->rs[slot] && cache->other_rate != in_rate) {
		switch_resample_destroy(&cache->rs[slot]); /* the shared slot now serves a different rate */
	}
	if (!cache->rs[slot]) {
		if (switch_resample_create(&cache->rs[slot], in_rate, out_rate, RESAMPLE_OUT_SAMPLES,
								   SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {
			cache->rs[slot] = NULL;
			return NULL;
		}
		cache->other_rate = in_rate;
	}
	return cache->rs[slot];
}

/* Receives audio that has been converted to the rate it is going to. */
typedef void (*emit_fn)(as_call_t *c, const uint8_t *data, size_t bytes);

/* Resample 16 bit mono pcm (samples long) from in_rate to out_rate and pass the result to emit.
 * The input is cut into pieces whose output always fits the resampler's buffer, so a long frame
 * is never truncated. Returns 0, or -1 if the resampler could not be created. */
static int resample_emit(as_call_t *c, as_rs_cache_t *cache, int in_rate, int out_rate,
						 const int16_t *pcm, size_t samples, emit_fn emit)
{
	/* a piece of this many samples produces about half of the output buffer at most */
	size_t max_in = (size_t)((unsigned long long)(RESAMPLE_OUT_SAMPLES / 2) * (unsigned)in_rate / (unsigned)out_rate);
	switch_audio_resampler_t *rs = rs_get(cache, in_rate, out_rate);

	if (!rs) {
		return -1;
	}
	if (max_in < 1) {
		max_in = 1;
	}
	while (samples) {
		size_t piece = samples < max_in ? samples : max_in;
		uint32_t out = switch_resample_process(rs, (int16_t *)pcm, (uint32_t)piece);

		if (out) {
			emit(c, (const uint8_t *)rs->to, (size_t)out * 2);
		}
		pcm += piece;
		samples -= piece;
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------
 * Caller to server
 * ---------------------------------------------------------------------------------------- */

/* Push as much queued data to the socket as it will take without waiting. */
static int flush_out(as_call_t *c)
{
	uint8_t tmp[4096];

	for (;;) {
		size_t n = as_ring_peek(&c->out, tmp, sizeof(tmp));
		ssize_t k;

		if (n == 0) {
			return 0;
		}
		k = send(c->fd, tmp, n, SEND_FLAGS);
		if (k > 0) {
			as_ring_skip(&c->out, (size_t)k);
			CADD(&c->bytes_sent, (unsigned long long)k);
			if ((size_t)k < n) {
				return 0; /* socket buffer is full, try again later */
			}
			continue;
		}
		if (k < 0 && errno == EINTR) {
			continue;
		}
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return 0;
		}
		c->broken = 1;
		return -1;
	}
}

/* Queue one audio frame (already at the server's rate) for the socket. */
static void emit_to_server(as_call_t *c, const uint8_t *payload, size_t plen)
{
	uint8_t hdr[AS_HEADER_LEN];

	if (plen == 0 || plen > AS_MAX_PAYLOAD) {
		return;
	}
	/* drop whole frames only, so the stream keeps its framing when the server is slow */
	if (as_ring_space(&c->out) < AS_HEADER_LEN + plen) {
		CADD(&c->frames_dropped, 1);
		return;
	}
	as_encode_header(hdr, (uint8_t)c->kind, (uint16_t)plen);
	as_ring_write(&c->out, hdr, AS_HEADER_LEN);
	as_ring_write(&c->out, payload, plen);
	CADD(&c->frames_sent, 1);
}

/* Queue a key press for the server: a DTMF frame whose payload is the digit as one ASCII byte
 * (0 to 9, * # and A to D), as Asterisk's AudioSocket sends it. */
static void send_dtmf(as_call_t *c, char digit)
{
	uint8_t frame[AS_HEADER_LEN + 1];
	uint8_t d = (uint8_t)digit;

	if (c->broken || as_encode_frame(frame, sizeof(frame), AS_KIND_DTMF, &d, 1) < 0) {
		return;
	}
	if (as_ring_space(&c->out) < sizeof(frame)) {
		return; /* the server is far behind; a key press is not worth blocking for */
	}
	as_ring_write(&c->out, frame, sizeof(frame));
	CADD(&c->dtmf_sent, 1);
	flush_out(c);
}

/* Queue one frame of 16 bit mono PCM for the server. in_rate is the rate of pcm. */
static void send_pcm(as_call_t *c, const int16_t *pcm, size_t bytes, int in_rate)
{
	if (c->broken || bytes < 2) {
		return;
	}
	if (in_rate == c->rate) {
		emit_to_server(c, (const uint8_t *)pcm, bytes);
	} else if (resample_emit(c, &c->out_rs, in_rate, c->rate, pcm, bytes / 2, emit_to_server) != 0) {
		CADD(&c->frames_dropped, 1);
	}
	flush_out(c);
}

/* The media bug hands us exactly one frame of the caller's audio per packet. We only look at it; the
 * audio the call carries is not changed.
 * The rate and channel count come from the call's read codec, not from the frame: the frame carries the
 * RTP clock rate, which is 8000 for G.722 although its audio is 16000 (switch_core_io.c sets the frame's
 * rate from the endpoint's frame), and may be missing altogether. The codec is asked each time because a
 * re-INVITE can change it. A stereo call is mixed down to mono. */
static switch_bool_t bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	as_call_t *c = (as_call_t *)user_data;

	if (type == SWITCH_ABC_TYPE_READ_REPLACE) {
		switch_frame_t *frame = switch_core_media_bug_get_read_replace_frame(bug);

		CADD(&c->bug_callbacks, 1);
		if (frame && frame->data && frame->datalen) {
			switch_codec_implementation_t impl;
			int16_t mono[SWITCH_RECOMMENDED_BUFFER_SIZE / 2];
			const int16_t *pcm = (const int16_t *)frame->data;
			int in_rate, channels;
			size_t samples;

			switch_core_session_get_read_impl(c->session, &impl);
			in_rate = (int)impl.actual_samples_per_second;
			channels = (int)impl.number_of_channels;
			if (in_rate < 1000 || in_rate > 192000) {
				in_rate = c->rate;
			}
			if (channels < 1) {
				channels = 1;
			}
			samples = frame->datalen / 2 / (size_t)channels;
			CADD(&c->bug_reads, 1);
			CADD(&c->bug_read_bytes, (unsigned long long)frame->datalen);
			if (channels > 1) {
				if (samples == 0 || samples > sizeof(mono) / sizeof(mono[0])) {
					CADD(&c->frames_dropped, 1);
					return SWITCH_TRUE;
				}
				as_downmix_mono(pcm, samples, channels, mono);
				pcm = mono;
			}
			send_pcm(c, pcm, samples * 2, in_rate);
		}
	}
	return SWITCH_TRUE;
}

/* ------------------------------------------------------------------------------------------
 * Server to caller
 * ---------------------------------------------------------------------------------------- */

/* Queue audio (already at the call's rate) in the jitter buffer for playback. */
static void emit_to_jitter(as_call_t *c, const uint8_t *data, size_t bytes)
{
	as_jitter_put(&c->jitter, data, bytes);
}

/* Audio from the server: convert to the call's rate if needed and queue it for playback. */
static void queue_server_audio(as_call_t *c, const int16_t *pcm, size_t bytes, int in_rate)
{
	CADD(&c->frames_in, 1);
	CADD(&c->bytes_in, (unsigned long long)bytes);
	if (!c->playback || bytes < 2) {
		return;
	}
	if (in_rate == c->write_rate) {
		emit_to_jitter(c, (const uint8_t *)pcm, bytes);
	} else {
		resample_emit(c, &c->in_rs, in_rate, c->write_rate, pcm, bytes / 2, emit_to_jitter);
	}
}

static void playback_teardown(as_call_t *c)
{
	c->underruns_acc += CGET(&c->jitter.underruns);
	c->playback = 0;
	if (c->codec_ready) {
		switch_core_codec_destroy(&c->wcodec);
		c->codec_ready = 0;
	}
	as_jitter_free(&c->jitter); /* safe when nothing was set up */
	rs_free_all(&c->in_rs);
}

/* Set the return path up for the call's current write codec: an L16 codec of its rate and packet time,
 * and a jitter buffer sized in packets. Playback needs a mono call; for others the server's audio is
 * accepted and discarded. Returns 0 when playback is on. */
static int playback_setup(switch_core_session_t *session, as_call_t *c)
{
	switch_codec_implementation_t wimpl;
	int ptime_ms;
	unsigned prefill_frames, max_frames;

	c->playback = 0;
	switch_core_session_get_write_impl(session, &wimpl);
	c->write_rate = (int)wimpl.actual_samples_per_second;
	c->write_channels = (int)wimpl.number_of_channels;
	c->frame_bytes = (size_t)wimpl.decoded_bytes_per_packet;
	c->write_us = wimpl.microseconds_per_packet > 0 ? (long long)wimpl.microseconds_per_packet : 20000;
	ptime_ms = (int)(c->write_us / 1000);
	if (ptime_ms < 1) {
		ptime_ms = 20;
	}
	if (c->frame_bytes < 2) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "audiosocket: the call has no write codec yet, audio from the server will be ignored\n");
		return -1;
	}
	if (c->write_channels != 1 || c->frame_bytes > PLAY_BUF_BYTES || c->frame_bytes % 2 != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "audiosocket: playback needs mono audio (the call has %d channels), audio from the server will be ignored\n",
						  c->write_channels);
		return -1;
	}
	prefill_frames = (unsigned)(c->jitter_ms / ptime_ms);
	if (prefill_frames < 1) {
		prefill_frames = 1;
	}
	max_frames = (unsigned)(c->max_queue_ms / ptime_ms);
	if (max_frames <= prefill_frames) {
		max_frames = prefill_frames + 1;
	}
	if (as_jitter_init(&c->jitter, c->frame_bytes, prefill_frames, max_frames) != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "audiosocket: could not set up playback, audio from the server will be ignored\n");
		return -1;
	}
	if (switch_core_codec_init(&c->wcodec, "L16", NULL, NULL, c->write_rate, ptime_ms, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
							   switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
		as_jitter_free(&c->jitter);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "audiosocket: could not set up playback, audio from the server will be ignored\n");
		return -1;
	}
	c->codec_ready = 1;
	c->playback = 1;
	return 0;
}

/* One playback tick: the next packet from the jitter buffer, or silence, written to the call.
 * buflen must be set: some endpoints (mod_loopback) copy the frame with buflen bytes of space, and a zero
 * buflen makes that copy overrun its buffer. */
static void play_tick(switch_core_session_t *session, as_call_t *c, switch_frame_t *wframe, uint8_t *wbuf)
{
	as_jitter_get(&c->jitter, wbuf);
	wframe->data = wbuf;
	wframe->buflen = PLAY_BUF_BYTES;
	wframe->channels = 1;
	wframe->datalen = (uint32_t)c->frame_bytes;
	wframe->samples = (uint32_t)(c->frame_bytes / 2);
	wframe->rate = (uint32_t)c->write_rate;
	wframe->codec = &c->wcodec;
	switch_core_session_write_frame(session, wframe, SWITCH_IO_FLAG_NONE, 0);
}

/* ------------------------------------------------------------------------------------------
 * Settings and metadata
 * ---------------------------------------------------------------------------------------- */

/* A whole number from a channel variable, held to the same limits as the config file. A value that is not
 * a number or is out of range is refused with a warning and the default is used. */
static int get_ranged_var(switch_core_session_t *session, switch_channel_t *channel, const char *name,
						  int def, int min, int max)
{
	const char *v = switch_channel_get_variable(channel, name);
	int out;

	if (zstr(v)) {
		return def;
	}
	if (as_parse_int(v, min, max, &out) != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "audiosocket: %s=[%s] is not a whole number from %d to %d, using %d\n",
						  name, v, min, max, def);
		return def;
	}
	return out;
}

/* Read autoload_configs/audiosocket.conf.xml into the shared settings. A missing file gives the built-in
 * defaults; an unknown name or a bad value is logged and skipped, and that setting keeps its default.
 * Calls already running keep the settings they started with. If stream is not NULL the result is also
 * written to it. Returns the number of problems found. */
static int load_config(switch_stream_handle_t *stream)
{
	as_config_t c;
	switch_xml_t xml, cfg, settings, param;
	int problems = 0;

	as_config_defaults(&c);
	xml = switch_xml_open_cfg("audiosocket.conf", &cfg, NULL);
	if (!xml) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "audiosocket: no audiosocket.conf found, using the built-in defaults\n");
	} else {
		settings = switch_xml_child(cfg, "settings");
		for (param = settings ? switch_xml_child(settings, "param") : NULL; param; param = param->next) {
			const char *name = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");
			int rc = as_config_set(&c, name, val);

			if (rc == AS_CONF_UNKNOWN) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "audiosocket.conf: unknown setting [%s] ignored\n", name);
				problems++;
			} else if (rc == AS_CONF_INVALID) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "audiosocket.conf: invalid value [%s] for [%s], keeping the default\n", val, name);
				problems++;
			}
		}
		switch_xml_free(xml);
	}

	stats_lock();
	g.config = c;
	stats_unlock();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "audiosocket: settings: rate %d, connect timeout %d ms, jitter %d ms, max queue %d ms, "
					  "send queue %d ms, stall timeout %d ms, dtmf %s, send-vars [%s]\n",
					  c.rate, c.connect_timeout_ms, c.jitter_ms, c.max_queue_ms, c.send_queue_ms,
					  c.stall_timeout_ms, c.dtmf ? "on" : "off", c.send_vars);
	if (stream) {
		stream->write_function(stream, "+OK audiosocket settings: rate %d, connect timeout %d ms, jitter %d ms, "
							   "max queue %d ms, send queue %d ms, stall timeout %d ms, dtmf %s, send-vars [%s]%s\n",
							   c.rate, c.connect_timeout_ms, c.jitter_ms, c.max_queue_ms, c.send_queue_ms,
							   c.stall_timeout_ms, c.dtmf ? "on" : "off", c.send_vars,
							   problems ? " (some settings were rejected, see the log)" : "");
		stream->write_function(stream, "FreeSWITCH keeps its XML in memory: after editing audiosocket.conf.xml run "
							   "reloadxml first, then audiosocket_reload.\n");
	}
	return problems;
}

/* Send the metadata frame: one JSON object holding the named channel variables ("name": null when a
 * variable is not set). Names that are not plain variable names are skipped with a warning. Nothing is
 * sent when no valid name is left, the list is too long, or the JSON would not fit. Returns -1 only if
 * the socket failed. */
static int send_metadata(switch_core_session_t *session, switch_channel_t *channel, int fd, const char *list)
{
	char names[AS_CONF_VARS_MAX + 1];
	char json[METADATA_MAX];
	uint8_t frame[AS_HEADER_LEN + METADATA_MAX];
	char *save = NULL, *tok;
	as_json_t j;
	int len, flen;

	if (strlen(list) >= sizeof(names)) {
		/* cutting it would turn the last name into a different one, which could expose another variable */
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "audiosocket: audiosocket_send_vars is longer than %d characters, no metadata sent\n",
						  AS_CONF_VARS_MAX);
		return 0;
	}
	snprintf(names, sizeof(names), "%s", list);
	as_json_init(&j, json, sizeof(json));
	for (tok = strtok_r(names, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		size_t n;

		while (*tok == ' ') {
			tok++;
		}
		n = strlen(tok);
		while (n > 0 && tok[n - 1] == ' ') {
			tok[--n] = '\0';
		}
		if (n == 0) {
			continue;
		}
		if (!as_var_name_ok(tok, n)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
							  "audiosocket: [%s] is not a usable variable name, left out of the metadata\n", tok);
			continue;
		}
		as_json_add(&j, tok, switch_channel_get_variable(channel, tok));
	}
	if (j.count == 0) {
		return 0;
	}
	len = as_json_finish(&j);
	if (len < 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "audiosocket: the metadata does not fit in %d bytes, not sent\n", METADATA_MAX);
		return 0;
	}
	flen = as_encode_frame(frame, sizeof(frame), AS_KIND_METADATA, (const uint8_t *)json, (size_t)len);
	if (flen < 0 || as_send_all(fd, frame, (size_t)flen, SEND_TIMEOUT_MS) != 0) {
		return -1;
	}
	return 0;
}

/* Asked by the connect every 100 ms: give up if the caller has hung up. */
static int connect_abort(void *ctx)
{
	return !switch_channel_ready((switch_channel_t *)ctx);
}

/* ------------------------------------------------------------------------------------------
 * The application
 * ---------------------------------------------------------------------------------------- */

SWITCH_STANDARD_APP(audiosocket_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *call_uuid = switch_core_session_get_uuid(session);
	char *mydata, *argv[4] = {0};
	int argc, port = 0, rate, timeout_ms, stall_timeout_ms;
	as_config_t cfg;
	const char *send_vars;
	char host[256] = "";
	uint8_t uuid_bin[AS_UUID_BYTES];
	uint8_t out[AS_HEADER_LEN + AS_UUID_BYTES];
	int16_t *payload_store = NULL; /* room for the largest frame the protocol allows; from malloc, so 16 bit aligned.
									  Only the pages a call actually uses are committed. */
	uint8_t net[4096];
	uint8_t wbuf[PLAY_BUF_BYTES];
	as_decoder_t dec;
	as_call_t call;
	as_pacer_t pacer;
	as_stall_t stall;
	switch_media_bug_t *bug = NULL;
	switch_frame_t wframe = {0};
	const char *status = "error";
	const char *var;
	int out_len, registered = 0, streaming = 0, server_gone = 0, draining = 0;
	int drain_empty_seen = 0, finish_ticks = 0, pb_ticks = 0;
	long long drain_deadline_us = 0;
	unsigned long frames_read = 0;
	unsigned long long final_trimmed = 0, final_dropped_full = 0;
	unsigned long final_underruns = 0;
	size_t queue_bytes;

	memset(&call, 0, sizeof(call));
	call.session = session;
	call.fd = -1;
	stats_call_started();

	if (zstr(data)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "audiosocket: usage: audiosocket <host:port> <uuid> [rate]\n");
		status = "bad_args";
		goto done;
	}
	mydata = switch_core_session_strdup(session, data);
	argc = (int)switch_separate_string(mydata, ' ', argv, 4);
	if (argc < 2 || as_parse_hostport(argv[0], host, sizeof(host), &port) != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "audiosocket: bad address, expected <host:port> <uuid> [rate], got [%s]\n", data);
		status = "bad_args";
		goto done;
	}
	if (as_uuid_parse(argv[1], uuid_bin) != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "audiosocket: [%s] is not a valid UUID\n", argv[1]);
		status = "bad_args";
		goto done;
	}

	/* Settings: the app argument or a channel variable wins over audiosocket.conf.xml, which wins over the
	 * built-in defaults. A copy of the shared settings is taken, so a reload never changes a running call. */
	stats_lock();
	cfg = g.config;
	stats_unlock();

	rate = cfg.rate;
	var = argc >= 3 ? argv[2] : switch_channel_get_variable(channel, "audiosocket_rate");
	if (!zstr(var)) {
		rate = atoi(var);
		if (as_rate_kind(rate) < 0) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
							  "audiosocket: unsupported rate [%s]\n", var);
			status = "bad_args";
			goto done;
		}
	}
	call.rate = rate;
	call.kind = as_rate_kind(rate);
	timeout_ms = get_ranged_var(session, channel, "audiosocket_connect_timeout_ms", cfg.connect_timeout_ms,
								AS_CONNECT_TIMEOUT_MIN, AS_CONNECT_TIMEOUT_MAX);
	call.jitter_ms = get_ranged_var(session, channel, "audiosocket_jitter_ms", cfg.jitter_ms,
									AS_JITTER_MS_MIN, AS_JITTER_MS_MAX);
	call.max_queue_ms = get_ranged_var(session, channel, "audiosocket_max_queue_ms", cfg.max_queue_ms,
									   AS_MAX_QUEUE_MS_MIN, AS_MAX_QUEUE_MS_MAX);
	stall_timeout_ms = get_ranged_var(session, channel, "audiosocket_stall_timeout_ms", cfg.stall_timeout_ms,
									  AS_STALL_TIMEOUT_MS_MIN, AS_STALL_TIMEOUT_MS_MAX);
	var = switch_channel_get_variable(channel, "audiosocket_dtmf");
	call.dtmf_enabled = zstr(var) ? cfg.dtmf : !switch_false(var); /* off for false, no, off or 0 */
	send_vars = switch_channel_get_variable(channel, "audiosocket_send_vars");
	if (zstr(send_vars)) {
		send_vars = cfg.send_vars;
	}
	if (!zstr(send_vars) && !strcasecmp(send_vars, "none")) {
		send_vars = ""; /* lets one call turn off a list set in the config file */
	}

	if (!switch_channel_test_flag(channel, CF_ANSWERED)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "audiosocket: the call is not answered; attaching to its audio will start early media. Run it after answer.\n");
	}

	call.fd = as_connect_abortable(host, port, timeout_ms, connect_abort, channel);
	if (call.fd < 0) {
		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
							  "audiosocket: the call ended while connecting to %s:%d\n", host, port);
			status = "ok";
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
							  "audiosocket: cannot connect to %s:%d within %d ms\n", host, port, timeout_ms);
			status = "connect_failed";
		}
		goto done;
	}

	out_len = as_encode_frame(out, sizeof(out), AS_KIND_UUID, uuid_bin, AS_UUID_BYTES);
	if (out_len < 0 || as_send_all(call.fd, out, (size_t)out_len, SEND_TIMEOUT_MS) != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "audiosocket: could not send the UUID frame to %s:%d\n", host, port);
		goto done;
	}
	if (!zstr(send_vars) && send_metadata(session, channel, call.fd, send_vars) != 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "audiosocket: could not send the metadata frame to %s:%d\n", host, port);
		goto done;
	}

	/* queue for audio waiting on the socket: send_queue_ms of audio plus frame headers */
	queue_bytes = (size_t)rate * 2 * (size_t)cfg.send_queue_ms / 1000;
	queue_bytes += queue_bytes / 20 + 1024;
	payload_store = (int16_t *)malloc(AS_MAX_PAYLOAD + 2);
	if (as_ring_init(&call.out, queue_bytes) != 0 || !payload_store) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "audiosocket: out of memory\n");
		goto done;
	}

	/* the return path; a call it cannot play to still streams its audio to the server */
	playback_setup(session, &call);
	as_pacer_init(&pacer, call.write_us > 0 ? call.write_us : 20000, MAX_PACE_BURST);
	as_stall_init(&stall, (long long)stall_timeout_ms * 1000);

	if (switch_core_media_bug_add(session, "audiosocket", NULL, bug_callback, &call, 0,
								  SMBF_READ_REPLACE, &bug) != SWITCH_STATUS_SUCCESS) {
		/* This also happens when the caller hangs up just after the connection was made: not an error. */
		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
							  "audiosocket: the call ended before the audio could be attached\n");
			status = "ok";
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
							  "audiosocket: could not attach to the call's audio\n");
		}
		goto done;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
					  "audiosocket: connected to %s:%d, uuid %s, rate %d, call %s (playback %s, %d Hz, %lld us packets; "
					  "jitter %d ms, max queue %d ms, stall timeout %d ms, dtmf %s, metadata [%s])\n",
					  host, port, argv[1], rate, call_uuid, call.playback ? "on" : "off", call.write_rate,
					  call.write_us, call.jitter_ms, call.max_queue_ms, stall_timeout_ms,
					  call.dtmf_enabled ? "on" : "off", zstr(send_vars) ? "" : send_vars);

	as_decoder_init(&dec, (uint8_t *)payload_store, AS_MAX_PAYLOAD);
	status = "ok";
	register_call(&call, argv[1], argv[0]);
	registered = 1;
	streaming = 1;

	/* Reading the call's frame wakes the loop, runs the media bug and notices hangup. The playback frames
	 * are then written according to the clock (the pacer), because the read can return at once. */
	while (switch_channel_ready(channel)) {
		switch_frame_t *read_frame = NULL;
		switch_status_t rs = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
		long long now;
		int due, k;

		if (!SWITCH_READ_ACCEPTABLE(rs)) {
			break; /* the call ended */
		}
		frames_read++;
		now = as_now_us();

		if (!draining && !server_gone) {
			/* what is left from a full socket buffer */
			if (flush_out(&call) < 0) {
				status = "server_closed";
				server_gone = 1;
			}
		}
		if (!draining && !server_gone && call.dtmf_enabled && switch_channel_has_dtmf(channel)) {
			switch_dtmf_t dtmf;

			memset(&dtmf, 0, sizeof(dtmf));
			while (switch_channel_dequeue_dtmf(channel, &dtmf) == SWITCH_STATUS_SUCCESS) {
				send_dtmf(&call, dtmf.digit);
			}
		}

		/* take what the server has sent; a burst is read over several rounds, and a flood of tiny frames
		 * is limited per tick so one call cannot hold its thread, the rest waits in the socket */
		if (!draining && !server_gone) {
			int round, frames_this_tick = 0;

			for (round = 0; round < RECV_ROUNDS_PER_TICK && !server_gone && frames_this_tick < MAX_FRAMES_PER_TICK;
				 round++) {
				/* a non-blocking read says "nothing yet" by itself, so no poll is needed first */
				ssize_t n = recv(call.fd, net, sizeof(net), MSG_DONTWAIT);
				size_t off = 0;

				if (n == 0) {
					status = "server_closed";
					server_gone = 1;
					break;
				}
				if (n < 0) {
					if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
						/* a reset or a timeout means the connection is gone; anything else is a real error */
						status = as_errno_is_disconnect(errno) ? "server_closed" : "error";
						server_gone = 1;
					}
					break;
				}
				while (off < (size_t)n) {
					size_t used = 0;
					int rc = as_decoder_feed(&dec, net + off, (size_t)n - off, &used);

					off += used;
					if (rc == AS_DECODE_ERROR) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
										  "audiosocket: frame from server larger than %d bytes, closing\n", AS_MAX_PAYLOAD);
						status = "error";
						server_gone = 1;
						break;
					}
					if (rc == AS_DECODE_FRAME) {
						int in_rate = as_kind_rate(dec.kind);

						frames_this_tick++;
						if (in_rate) {
							queue_server_audio(&call, payload_store, dec.len, in_rate);
						} else if (dec.kind == AS_KIND_HANGUP) {
							status = "server_hangup";
							server_gone = 1;
							break;
						} else if (dec.kind == AS_KIND_ERROR) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
											  "audiosocket: error frame from server (code 0x%02x), ignored\n",
											  dec.len ? ((uint8_t *)payload_store)[0] : 0);
						}
						/* DTMF and other frames from the server are ignored */
					}
				}
				if (!server_gone && (size_t)n < sizeof(net)) {
					break; /* the socket is drained, no need for another empty read */
				}
			}
			if (call.broken) {
				status = "server_closed";
				server_gone = 1;
			}
		}

		/* a server that has stopped accepting data (but has not closed) is given up on */
		if (!draining && !server_gone &&
			as_stall_check(&stall, now, as_ring_level(&call.out), CGET(&call.bytes_sent))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
							  "audiosocket: the server at %s:%d has accepted no data for %d ms, giving up\n",
							  host, port, stall_timeout_ms);
			status = "server_stalled";
			server_gone = 1;
		}

		/* The server is gone. Audio it has already sent is still played; then the call moves on. */
		if (server_gone && !draining) {
			size_t pending = call.playback ? as_jitter_pending(&call.jitter) : 0;

			if (pending == 0) {
				break;
			}
			draining = 1;
			as_jitter_drain(&call.jitter);
			drain_deadline_us = now + ((long long)call.max_queue_ms + 1000) * 1000;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
							  "audiosocket: server gone (%s), playing out %lu ms of audio it had sent\n", status,
							  (unsigned long)(pending * 1000ULL / ((unsigned long long)call.write_rate * 2)));
		}

		/* playback, by the clock */
		due = as_pacer_due(&pacer, now);
		if (due == 0) {
			switch_sleep(1000); /* the read returned at once; do not spin */
		}
		for (k = 0; k < due; k++) {
			if (call.playback) {
				play_tick(session, &call, &wframe, wbuf);
			}
		}
		if (draining) {
			if (as_jitter_pending(&call.jitter) == 0) {
				if (!drain_empty_seen) {
					drain_empty_seen = 1;
					finish_ticks = DRAIN_EXTRA_TICKS;
				}
				finish_ticks -= due;
				if (finish_ticks <= 0) {
					break;
				}
			}
			if (now >= drain_deadline_us) {
				break;
			}
		}

		/* if the call's write codec has changed (a re-INVITE), set the return path up again for the new one */
		if (call.playback && !draining && ++pb_ticks >= PLAYBACK_CHECK_TICKS) {
			switch_codec_implementation_t wimpl;

			pb_ticks = 0;
			switch_core_session_get_write_impl(session, &wimpl);
			if ((int)wimpl.actual_samples_per_second != call.write_rate ||
				(size_t)wimpl.decoded_bytes_per_packet != call.frame_bytes ||
				(int)wimpl.number_of_channels != call.write_channels) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
								  "audiosocket: the call's audio format changed, setting playback up again\n");
				playback_teardown(&call);
				playback_setup(session, &call);
				as_pacer_init(&pacer, call.write_us > 0 ? call.write_us : 20000, MAX_PACE_BURST);
			}
		}
	}

done:
	if (bug) {
		switch_core_media_bug_remove(session, &bug);
	}
	if (registered) {
		unregister_call(&call); /* before the call's memory goes away */
	}
	if (call.fd >= 0) {
		if (streaming && !server_gone) {
			/* the call ended: send what is still queued, then tell the server, best effort */
			flush_out(&call);
			out_len = as_encode_frame(out, sizeof(out), AS_KIND_HANGUP, NULL, 0);
			if (out_len > 0 && as_ring_level(&call.out) == 0) {
				as_send_all(call.fd, out, (size_t)out_len, 200);
			}
		}
		close(call.fd);
	}

	final_underruns = call_underruns(&call);
	final_trimmed = CGET(&call.jitter.trimmed_bytes);
	final_dropped_full = CGET(&call.jitter.dropped_full_bytes);
	stats_add_totals(&call);
	playback_teardown(&call);
	rs_free_all(&call.out_rs);
	as_ring_free(&call.out);
	free(payload_store);

	set_status(channel, status);
	switch_channel_set_variable_printf(channel, "audiosocket_frames_sent", "%lu", call.frames_sent);
	switch_channel_set_variable_printf(channel, "audiosocket_frames_dropped", "%lu", call.frames_dropped);
	switch_channel_set_variable_printf(channel, "audiosocket_frames_received", "%lu", call.frames_in);
	switch_channel_set_variable_printf(channel, "audiosocket_underruns", "%lu", final_underruns);
	switch_channel_set_variable_printf(channel, "audiosocket_dtmf_sent", "%lu", call.dtmf_sent);
	if (streaming) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "audiosocket: finished, status %s. To server: %lu frames (%llu bytes), %lu dropped. "
						  "From server: %lu frames (%llu bytes), %lu underruns, %llu bytes trimmed, %llu bytes dropped. "
						  "Key presses sent: %lu. Call frames read %lu\n",
						  status, call.frames_sent, call.bytes_sent, call.frames_dropped,
						  call.frames_in, call.bytes_in, final_underruns, final_trimmed, final_dropped_full,
						  call.dtmf_sent, frames_read);
	}
}

/* ------------------------------------------------------------------------------------------
 * Commands
 * ---------------------------------------------------------------------------------------- */

typedef struct {
	char uuid[40];
	char peer[300];
	int rate;
	double secs;
	unsigned long sent, dropped, in, under, play_ms, out_bytes;
} as_snap_t;

/* fs_cli: audiosocket_stats          summary
 *         audiosocket_stats list     one line per active stream
 * The numbers are copied while the lock is held and printed after it is released, so a slow or stalled
 * client reading the output cannot hold up calls starting and ending. */
SWITCH_STANDARD_API(audiosocket_stats_function)
{
	int list = !zstr(cmd) && !strcasecmp(cmd, "list");
	switch_time_t now = switch_micro_time_now();
	as_call_t *c;

	if (list) {
		as_snap_t *snap;
		size_t count = 0, cap, n = 0, i;

		stats_lock();
		for (c = g.head; c; c = c->next) {
			count++;
		}
		stats_unlock();
		cap = count + 32; /* calls may start between the count and the copy */
		snap = (as_snap_t *)malloc(cap * sizeof(*snap));
		if (!snap) {
			stream->write_function(stream, "-ERR out of memory\n");
			return SWITCH_STATUS_SUCCESS;
		}
		stats_lock();
		for (c = g.head; c && n < cap; c = c->next, n++) {
			as_snap_t *s = &snap[n];

			snprintf(s->uuid, sizeof(s->uuid), "%s", c->uuid);
			snprintf(s->peer, sizeof(s->peer), "%s", c->peer);
			s->rate = c->rate;
			s->secs = (double)(now - c->started) / 1000000.0;
			s->sent = CGET(&c->frames_sent);
			s->dropped = CGET(&c->frames_dropped);
			s->in = CGET(&c->frames_in);
			s->under = call_underruns(c);
			s->play_ms = (c->playback && c->write_rate > 0)
							 ? (unsigned long)(as_jitter_pending(&c->jitter) * 1000ULL / ((unsigned long long)c->write_rate * 2))
							 : 0;
			s->out_bytes = (unsigned long)as_ring_level(&c->out);
		}
		stats_unlock();

		stream->write_function(stream, "%-36s  %-22s %6s %8s %9s %8s %9s %8s %8s %8s\n",
							   "uuid", "server", "rate", "seconds", "sent", "dropped", "received", "underrun",
							   "play_ms", "out_bytes");
		for (i = 0; i < n; i++) {
			/* FreeSWITCH's printf does not know %zu, so sizes are passed as unsigned long */
			stream->write_function(stream, "%-36s  %-22s %6d %8.1f %9lu %8lu %9lu %8lu %8lu %8lu\n",
								   snap[i].uuid, snap[i].peer, snap[i].rate, snap[i].secs, snap[i].sent,
								   snap[i].dropped, snap[i].in, snap[i].under, snap[i].play_ms, snap[i].out_bytes);
		}
		stream->write_function(stream, "%lu active\n", (unsigned long)n);
		free(snap);
	} else {
		as_totals_t t;
		unsigned long long active = 0, live_sent = 0, live_dropped = 0, live_bytes = 0, live_in = 0, live_under = 0;
		switch_time_t since;

		stats_lock();
		t = g.t;
		since = g.since;
		for (c = g.head; c; c = c->next) {
			active++;
			live_sent += CGET(&c->frames_sent);
			live_dropped += CGET(&c->frames_dropped);
			live_bytes += CGET(&c->bytes_sent);
			live_in += CGET(&c->frames_in);
			live_under += call_underruns(c);
		}
		stats_unlock();

		stream->write_function(stream, "mod_audiosocket %s, running %.0f s\n", AUDIOSOCKET_VERSION,
							   (double)(now - since) / 1000000.0);
		stream->write_function(stream, "active streams:        %llu\n", active);
		stream->write_function(stream, "calls started:         %llu\n", t.calls_started);
		stream->write_function(stream, "frames sent to server: %llu (finished calls and active ones)\n",
							   t.frames_sent + live_sent);
		stream->write_function(stream, "frames dropped:        %llu\n", t.frames_dropped + live_dropped);
		stream->write_function(stream, "audio bytes sent:      %llu\n", t.bytes_sent + live_bytes);
		stream->write_function(stream, "frames from server:    %llu\n", t.frames_in + live_in);
		stream->write_function(stream, "playback underruns:    %llu\n", t.underruns + live_under);
		stream->write_function(stream, "key presses sent:      %llu (finished calls)\n", t.dtmf);
		stream->write_function(stream, "ended: ok %llu, server_hangup %llu, server_closed %llu, server_stalled %llu, "
							   "connect_failed %llu, bad_args %llu, error %llu\n",
							   t.ended_ok, t.ended_server_hangup, t.ended_server_closed, t.ended_server_stalled,
							   t.ended_connect_failed, t.ended_bad_args, t.ended_error);
	}
	return SWITCH_STATUS_SUCCESS;
}

/* fs_cli: audiosocket_reload   read autoload_configs/audiosocket.conf.xml again; calls in progress are not affected */
SWITCH_STANDARD_API(audiosocket_reload_function)
{
	load_config(stream);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audiosocket_load)
{
	switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;

	memset(&g, 0, sizeof(g));
	switch_mutex_init(&g.mutex, SWITCH_MUTEX_NESTED, pool);
	g.since = switch_micro_time_now();
	as_config_defaults(&g.config);
	load_config(NULL);

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, AUDIOSOCKET_APP_NAME, AUDIOSOCKET_APP_SYNOPSIS, AUDIOSOCKET_APP_DESC,
				   audiosocket_function, "<host:port> <uuid> [rate]", SAF_NONE);
	SWITCH_ADD_API(api_interface, "audiosocket_stats", "Show mod_audiosocket statistics",
				   audiosocket_stats_function, "[list]");
	SWITCH_ADD_API(api_interface, "audiosocket_reload", "Read audiosocket.conf.xml again",
				   audiosocket_reload_function, "");
	switch_console_set_complete("add audiosocket_stats list");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audiosocket %s loaded\n", AUDIOSOCKET_VERSION);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audiosocket_shutdown)
{
	int i, busy = 0;

	/* Calls that are running hold pointers into this module's state. Give them a few seconds to end; if
	 * any is left, refuse to unload rather than pull the module out from under them. */
	for (i = 0; i < 50; i++) {
		stats_lock();
		busy = g.head != NULL;
		stats_unlock();
		if (!busy) {
			break;
		}
		switch_sleep(100000);
	}
	if (busy) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "audiosocket: calls are still using the module, it cannot be unloaded now\n");
		return SWITCH_STATUS_NOUNLOAD;
	}
	switch_console_set_complete("del audiosocket_stats");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audiosocket unloaded\n");
	return SWITCH_STATUS_SUCCESS;
}
