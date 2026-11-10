/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 */
#include "audiosocket_proto.h"

#include <string.h>

static const int rate_table[] = {8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000, 192000};
#define RATE_COUNT ((int)(sizeof(rate_table) / sizeof(rate_table[0])))

int as_kind_rate(uint8_t kind)
{
	if (kind < AS_KIND_AUDIO_8K || kind >= AS_KIND_AUDIO_8K + RATE_COUNT) {
		return 0;
	}
	return rate_table[kind - AS_KIND_AUDIO_8K];
}

int as_rate_kind(int rate_hz)
{
	int i;
	for (i = 0; i < RATE_COUNT; i++) {
		if (rate_table[i] == rate_hz) {
			return AS_KIND_AUDIO_8K + i;
		}
	}
	return -1;
}

void as_encode_header(uint8_t *out, uint8_t kind, uint16_t payload_len)
{
	out[0] = kind;
	out[1] = (uint8_t)(payload_len >> 8);
	out[2] = (uint8_t)(payload_len & 0xff);
}

int as_encode_frame(uint8_t *out, size_t cap, uint8_t kind, const uint8_t *payload, size_t payload_len)
{
	if (payload_len > AS_MAX_PAYLOAD || cap < AS_HEADER_LEN + payload_len) {
		return -1;
	}
	as_encode_header(out, kind, (uint16_t)payload_len);
	if (payload_len) {
		memcpy(out + AS_HEADER_LEN, payload, payload_len);
	}
	return (int)(AS_HEADER_LEN + payload_len);
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int as_uuid_parse(const char *str, uint8_t out[AS_UUID_BYTES])
{
	size_t len, i;
	int dashed, nibbles = 0, hi = -1;

	if (!str) {
		return -1;
	}
	len = strlen(str);
	if (len == 36) {
		dashed = 1;
	} else if (len == 32) {
		dashed = 0;
	} else {
		return -1;
	}

	for (i = 0; i < len; i++) {
		int v;
		if (dashed && (i == 8 || i == 13 || i == 18 || i == 23)) {
			if (str[i] != '-') {
				return -1;
			}
			continue;
		}
		v = hex_val(str[i]);
		if (v < 0) {
			return -1;
		}
		if (hi < 0) {
			hi = v;
		} else {
			out[nibbles / 2] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
		nibbles++;
	}
	return nibbles == 32 ? 0 : -1;
}

void as_decoder_init(as_decoder_t *d, uint8_t *buf, size_t cap)
{
	memset(d, 0, sizeof(*d));
	d->buf = buf;
	d->cap = cap;
}

int as_decoder_feed(as_decoder_t *d, const uint8_t *data, size_t n, size_t *consumed)
{
	size_t used = 0, take;

	*consumed = 0;
	if (d->failed) {
		return AS_DECODE_ERROR;
	}
	if (d->done) { /* previous frame was delivered, start the next one */
		d->hdr_have = 0;
		d->hdr_parsed = 0;
		d->pay_have = 0;
		d->done = 0;
	}

	while (d->hdr_have < AS_HEADER_LEN && used < n) {
		d->hdr[d->hdr_have++] = data[used++];
	}
	if (d->hdr_have < AS_HEADER_LEN) {
		*consumed = used;
		return AS_DECODE_NEED_MORE;
	}

	if (!d->hdr_parsed) {
		d->kind = d->hdr[0];
		d->len = (uint16_t)((d->hdr[1] << 8) | d->hdr[2]);
		if (d->len > d->cap) {
			d->failed = 1;
			*consumed = used;
			return AS_DECODE_ERROR;
		}
		d->hdr_parsed = 1;
	}

	take = (size_t)d->len - d->pay_have;
	if (take > n - used) {
		take = n - used;
	}
	if (take) {
		memcpy(d->buf + d->pay_have, data + used, take);
		d->pay_have += take;
		used += take;
	}
	*consumed = used;

	if (d->pay_have == d->len) {
		d->done = 1;
		return AS_DECODE_FRAME;
	}
	return AS_DECODE_NEED_MORE;
}
