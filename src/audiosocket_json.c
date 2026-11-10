/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 */
#include "audiosocket_json.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Room is always kept for the closing brace and the terminating NUL. */
static void put(as_json_t *j, const char *s, size_t n)
{
	if (j->overflow) {
		return;
	}
	if (j->len + n + 2 > j->cap) {
		j->overflow = 1;
		return;
	}
	memcpy(j->buf + j->len, s, n);
	j->len += n;
}

/* Length (2 to 4) of the well-formed UTF-8 sequence that starts at s, or 0 if there is none.
 * Rejects overlong forms, surrogates, values above U+10FFFF and truncated sequences (RFC 3629).
 * s ends at a NUL, which is never a continuation byte, so this never reads past it. */
static int utf8_seq_len(const unsigned char *s)
{
#define CONT(b) (((b) & 0xC0) == 0x80)
	unsigned char b0 = s[0];

	if (b0 >= 0xC2 && b0 <= 0xDF) {
		return CONT(s[1]) ? 2 : 0;
	}
	if (b0 >= 0xE0 && b0 <= 0xEF) {
		if (!CONT(s[1]) || !CONT(s[2])) {
			return 0;
		}
		if (b0 == 0xE0 && s[1] < 0xA0) {
			return 0; /* overlong */
		}
		if (b0 == 0xED && s[1] > 0x9F) {
			return 0; /* a UTF-16 surrogate */
		}
		return 3;
	}
	if (b0 >= 0xF0 && b0 <= 0xF4) {
		if (!CONT(s[1]) || !CONT(s[2]) || !CONT(s[3])) {
			return 0;
		}
		if (b0 == 0xF0 && s[1] < 0x90) {
			return 0; /* overlong */
		}
		if (b0 == 0xF4 && s[1] > 0x8F) {
			return 0; /* above U+10FFFF */
		}
		return 4;
	}
	return 0;
#undef CONT
}

/* Text is sent as UTF-8. Anything that is not well-formed UTF-8 (the value may come from a caller) is
 * replaced by U+FFFD, so a strict JSON parser on the server accepts the whole frame. */
static void put_escaped(as_json_t *j, const char *str)
{
	const unsigned char *s = (const unsigned char *)str;

	while (*s) {
		unsigned char ch = *s;
		char esc[8];

		if (ch >= 0x80) {
			int n = utf8_seq_len(s);

			if (n) {
				put(j, (const char *)s, (size_t)n);
				s += n;
			} else {
				put(j, "\xEF\xBF\xBD", 3);
				s++;
			}
			continue;
		}
		switch (ch) {
		case '"':
			put(j, "\\\"", 2);
			break;
		case '\\':
			put(j, "\\\\", 2);
			break;
		case '\b':
			put(j, "\\b", 2);
			break;
		case '\f':
			put(j, "\\f", 2);
			break;
		case '\n':
			put(j, "\\n", 2);
			break;
		case '\r':
			put(j, "\\r", 2);
			break;
		case '\t':
			put(j, "\\t", 2);
			break;
		default:
			if (ch < 0x20) {
				snprintf(esc, sizeof(esc), "\\u%04x", ch);
				put(j, esc, 6);
			} else {
				put(j, (const char *)&ch, 1);
			}
		}
		s++;
	}
}

void as_json_init(as_json_t *j, char *buf, size_t cap)
{
	j->buf = buf;
	j->cap = cap;
	j->len = 0;
	j->count = 0;
	j->overflow = 0;
	if (cap < 3) {
		j->overflow = 1;
		return;
	}
	buf[0] = '{';
	j->len = 1;
}

void as_json_add(as_json_t *j, const char *name, const char *value)
{
	if (j->overflow) {
		return;
	}
	if (j->count) {
		put(j, ",", 1);
	}
	put(j, "\"", 1);
	put_escaped(j, name);
	put(j, "\":", 2);
	if (value) {
		put(j, "\"", 1);
		put_escaped(j, value);
		put(j, "\"", 1);
	} else {
		put(j, "null", 4);
	}
	j->count++;
}

int as_json_finish(as_json_t *j)
{
	if (j->overflow) {
		return -1;
	}
	j->buf[j->len++] = '}';
	j->buf[j->len] = '\0';
	return (int)j->len;
}

int as_var_name_ok(const char *name, size_t len)
{
	size_t i;

	if (!name || len < 1 || len > 64) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		unsigned char ch = (unsigned char)name[i];
		if (!(isalnum(ch) || ch == '_' || ch == '.' || ch == '-')) {
			return 0;
		}
	}
	return 1;
}
