/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 *
 * Builds a flat JSON object of string values in a caller-supplied buffer, for the metadata frame.
 * Strings are escaped (quote, backslash, control characters); bytes of 0x80 and above are passed through
 * unchanged, so UTF-8 text stays UTF-8. Plain C99, no FreeSWITCH dependency, no allocation.
 */
#ifndef AUDIOSOCKET_JSON_H
#define AUDIOSOCKET_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	char *buf;
	size_t cap;
	size_t len;
	int count;    /* members added */
	int overflow; /* something did not fit: as_json_finish then fails */
} as_json_t;

/* Start an object in buf (cap bytes). */
void as_json_init(as_json_t *j, char *buf, size_t cap);

/* Add "name": "value", or "name": null when value is NULL. Nothing is added once the buffer has overflowed. */
void as_json_add(as_json_t *j, const char *name, const char *value);

/* Close the object and NUL terminate it. Returns its length, or -1 if anything did not fit
 * (the buffer content is then not valid JSON and must not be sent). */
int as_json_finish(as_json_t *j);

/* 1 if name is 1 to 64 characters of letters, digits, '_', '.' or '-'; 0 otherwise. */
int as_var_name_ok(const char *name, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AUDIOSOCKET_JSON_H */
