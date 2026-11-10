/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 */
#define _POSIX_C_SOURCE 200809L
#include "audiosocket_conf.h"

#include "audiosocket_proto.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void as_config_defaults(as_config_t *c)
{
	memset(c, 0, sizeof(*c));
	c->rate = 8000;
	c->connect_timeout_ms = 2000;
	c->jitter_ms = 200;
	c->max_queue_ms = 10000;
	c->send_queue_ms = 500;
	c->stall_timeout_ms = 10000;
	c->dtmf = 1;
	c->send_vars[0] = '\0';
}

int as_parse_bool(const char *s, int *out)
{
	if (!s) {
		return -1;
	}
	if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on") || !strcmp(s, "1")) {
		*out = 1;
		return 0;
	}
	if (!strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcasecmp(s, "off") || !strcmp(s, "0")) {
		*out = 0;
		return 0;
	}
	return -1;
}

int as_parse_int(const char *s, int min, int max, int *out)
{
	char *end = NULL;
	long v;

	if (!s) {
		return -1;
	}
	while (isspace((unsigned char)*s)) {
		s++;
	}
	if (*s == '\0') {
		return -1;
	}
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno != 0 || end == s) {
		return -1;
	}
	while (isspace((unsigned char)*end)) {
		end++;
	}
	if (*end != '\0' || v < min || v > max) {
		return -1;
	}
	*out = (int)v;
	return 0;
}

int as_send_vars_valid(const char *s)
{
	if (!s) {
		return 0;
	}
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		if (!(isalnum(ch) || ch == '_' || ch == '.' || ch == '-' || ch == ',' || ch == ' ')) {
			return 0;
		}
	}
	return 1;
}

int as_config_set(as_config_t *c, const char *name, const char *value)
{
	int v;

	if (!c || !name) {
		return AS_CONF_UNKNOWN;
	}
	if (!value) {
		value = "";
	}
	if (!strcmp(name, "default-rate")) {
		if (as_parse_int(value, 1, 1000000, &v) != 0 || as_rate_kind(v) < 0) {
			return AS_CONF_INVALID;
		}
		c->rate = v;
	} else if (!strcmp(name, "connect-timeout-ms")) {
		if (as_parse_int(value, AS_CONNECT_TIMEOUT_MIN, AS_CONNECT_TIMEOUT_MAX, &v) != 0) {
			return AS_CONF_INVALID;
		}
		c->connect_timeout_ms = v;
	} else if (!strcmp(name, "jitter-buffer-ms")) {
		if (as_parse_int(value, AS_JITTER_MS_MIN, AS_JITTER_MS_MAX, &v) != 0) {
			return AS_CONF_INVALID;
		}
		c->jitter_ms = v;
	} else if (!strcmp(name, "max-queue-ms")) {
		if (as_parse_int(value, AS_MAX_QUEUE_MS_MIN, AS_MAX_QUEUE_MS_MAX, &v) != 0) {
			return AS_CONF_INVALID;
		}
		c->max_queue_ms = v;
	} else if (!strcmp(name, "send-queue-ms")) {
		if (as_parse_int(value, AS_SEND_QUEUE_MS_MIN, AS_SEND_QUEUE_MS_MAX, &v) != 0) {
			return AS_CONF_INVALID;
		}
		c->send_queue_ms = v;
	} else if (!strcmp(name, "stall-timeout-ms")) {
		if (as_parse_int(value, AS_STALL_TIMEOUT_MS_MIN, AS_STALL_TIMEOUT_MS_MAX, &v) != 0) {
			return AS_CONF_INVALID;
		}
		c->stall_timeout_ms = v;
	} else if (!strcmp(name, "dtmf")) {
		if (as_parse_bool(value, &v) != 0) {
			return AS_CONF_INVALID;
		}
		c->dtmf = v;
	} else if (!strcmp(name, "send-vars")) {
		if (!as_send_vars_valid(value) || strlen(value) >= AS_CONF_VARS_MAX) {
			return AS_CONF_INVALID;
		}
		strcpy(c->send_vars, value);
	} else {
		return AS_CONF_UNKNOWN;
	}
	return AS_CONF_OK;
}
