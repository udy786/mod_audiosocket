/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 *
 * AudioSocket wire protocol: frame encoder, streaming decoder, UUID parsing
 * and the sample-rate table. Plain C99, no FreeSWITCH dependency, so it can be
 * built and unit-tested anywhere.
 *
 * Frame layout: 1 byte kind, 2 bytes payload length (big endian), payload.
 *
 * The kind codes below are the AudioSocket protocol's: hangup 0x00, UUID 0x01,
 * DTMF 0x03, error 0xFF, and audio 0x10 to 0x18 for the nine sample rates.
 */
#ifndef AUDIOSOCKET_PROTO_H
#define AUDIOSOCKET_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AS_HEADER_LEN 3
#define AS_MAX_PAYLOAD 65535
#define AS_UUID_BYTES 16

/* Frame kinds. Audio kinds are consecutive, one per sample rate. */
enum {
	AS_KIND_HANGUP = 0x00,
	AS_KIND_UUID = 0x01,
	AS_KIND_DTMF = 0x03,
	AS_KIND_AUDIO_8K = 0x10, /* signed linear 16 bit, mono, 8000 Hz */
	AS_KIND_AUDIO_12K = 0x11,
	AS_KIND_AUDIO_16K = 0x12,
	AS_KIND_AUDIO_24K = 0x13,
	AS_KIND_AUDIO_32K = 0x14,
	AS_KIND_AUDIO_44K = 0x15, /* 44100 Hz */
	AS_KIND_AUDIO_48K = 0x16,
	AS_KIND_AUDIO_96K = 0x17,
	AS_KIND_AUDIO_192K = 0x18,
	/* Optional extension to the base protocol: a JSON object of channel variables, sent once after the
	 * UUID frame, only when the dialplan asks for it (audiosocket_send_vars). */
	AS_KIND_METADATA = 0x40,
	AS_KIND_ERROR = 0xff
};

/* Return the sample rate in Hz for an audio kind, or 0 if kind is not audio. */
int as_kind_rate(uint8_t kind);

/* Return the audio kind for a sample rate in Hz, or -1 if the rate is unsupported. */
int as_rate_kind(int rate_hz);

/* Write a frame header (3 bytes) into out. */
void as_encode_header(uint8_t *out, uint8_t kind, uint16_t payload_len);

/* Write a whole frame (header + payload) into out of capacity cap.
 * Returns the total length written, or -1 if cap is too small or len > AS_MAX_PAYLOAD. */
int as_encode_frame(uint8_t *out, size_t cap, uint8_t kind, const uint8_t *payload, size_t payload_len);

/* Parse a UUID string ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" or 32 hex digits)
 * into 16 bytes. Returns 0 on success, -1 if the string is not a valid UUID. */
int as_uuid_parse(const char *str, uint8_t out[AS_UUID_BYTES]);

/* Streaming decoder. Bytes can arrive in any split (TCP gives no frame
 * boundaries). The caller supplies the payload buffer. */
typedef struct {
	uint8_t hdr[AS_HEADER_LEN];
	size_t hdr_have;
	int hdr_parsed;
	int done;
	int failed;
	uint8_t kind;
	uint16_t len;
	size_t pay_have;
	uint8_t *buf;
	size_t cap;
} as_decoder_t;

#define AS_DECODE_ERROR (-1)
#define AS_DECODE_NEED_MORE 0
#define AS_DECODE_FRAME 1

void as_decoder_init(as_decoder_t *d, uint8_t *buf, size_t cap);

/* Feed n bytes. Sets *consumed to how many bytes were used. Returns:
 *   AS_DECODE_FRAME     a whole frame is ready: d->kind, d->len, d->buf are valid
 *                       until the next call; feed the remaining bytes (n - *consumed) again
 *   AS_DECODE_NEED_MORE all bytes were consumed, the frame is not complete yet
 *   AS_DECODE_ERROR     the frame is larger than the buffer; the stream is unusable */
int as_decoder_feed(as_decoder_t *d, const uint8_t *data, size_t n, size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif /* AUDIOSOCKET_PROTO_H */
