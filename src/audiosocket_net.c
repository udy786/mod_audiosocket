/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 */
#define _POSIX_C_SOURCE 200809L
#include "audiosocket_net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define AS_SEND_FLAGS MSG_NOSIGNAL
#else
#define AS_SEND_FLAGS 0
#endif

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0) {
		return -1;
	}
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd p;
	int rc;

	p.fd = fd;
	p.events = events;
	p.revents = 0;
	do {
		rc = poll(&p, 1, timeout_ms);
	} while (rc < 0 && errno == EINTR);
	return rc;
}

int as_parse_hostport(const char *s, char *host, size_t host_cap, int *port)
{
	const char *colon, *portstr;
	size_t hlen;
	long p = 0;

	if (!s || !host || !port || host_cap == 0) {
		return -1;
	}
	if (s[0] == '[') { /* [ipv6]:port */
		const char *end = strchr(s, ']');
		if (!end || end == s + 1 || end[1] != ':') {
			return -1;
		}
		hlen = (size_t)(end - s - 1);
		colon = end + 1;
		if (hlen >= host_cap) {
			return -1;
		}
		memcpy(host, s + 1, hlen);
	} else {
		colon = strrchr(s, ':');
		if (!colon || colon == s) {
			return -1;
		}
		hlen = (size_t)(colon - s);
		if (memchr(s, ':', hlen)) { /* a bare IPv6 address needs brackets */
			return -1;
		}
		if (hlen >= host_cap) {
			return -1;
		}
		memcpy(host, s, hlen);
	}
	host[hlen] = '\0';

	portstr = colon + 1;
	if (*portstr == '\0') {
		return -1;
	}
	for (; *portstr; portstr++) {
		if (*portstr < '0' || *portstr > '9') {
			return -1;
		}
		p = p * 10 + (*portstr - '0');
		if (p > 65535) {
			return -1;
		}
	}
	if (p < 1) {
		return -1;
	}
	*port = (int)p;
	return 0;
}

int as_connect_abortable(const char *host, int port, int timeout_ms, int (*should_abort)(void *), void *ctx)
{
	char portstr[16];
	struct addrinfo hints, *res = NULL, *ai;
	int fd = -1, rc;
	long long deadline;

	if (!host || port < 1 || port > 65535 || timeout_ms < 1) {
		return -1;
	}
	deadline = now_ms() + timeout_ms;
	snprintf(portstr, sizeof(portstr), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	/* A literal IP address needs no lookup, so ask for exactly that first: it can never wait for DNS.
	 * Only a real host name falls through to the resolver, which can block for longer than timeout_ms. */
	hints.ai_flags = AI_NUMERICHOST;
	rc = getaddrinfo(host, portstr, &hints, &res);
	if (rc != 0) {
		hints.ai_flags = 0;
		if (getaddrinfo(host, portstr, &hints, &res) != 0) {
			return -1;
		}
	}

	for (ai = res; ai; ai = ai->ai_next) {
		long long left = deadline - now_ms();

		if (left <= 0) {
			break;
		}
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			continue;
		}
		/* Do not let a child process started by FreeSWITCH (system, bg_system, scripts) inherit the
		 * connection: it would keep the bot's socket open after the call ended. */
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
		if (set_nonblock(fd) < 0) {
			close(fd);
			fd = -1;
			continue;
		}
		rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (rc == 0) {
			break;
		}
		if (errno == EINPROGRESS) {
			/* wait in slices so a caller that has hung up is noticed within 100 ms */
			int ready = 0, aborted = 0;

			while (left > 0) {
				int slice = left > 100 ? 100 : (int)left;

				if (wait_fd(fd, POLLOUT, slice) > 0) {
					ready = 1;
					break;
				}
				if (should_abort && should_abort(ctx)) {
					aborted = 1;
					break;
				}
				left = deadline - now_ms();
			}
			if (ready) {
				int err = 0;
				socklen_t elen = sizeof(err);
				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
					break;
				}
			}
			if (aborted) {
				close(fd);
				fd = -1;
				break;
			}
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd >= 0) {
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		/* Find a server that has vanished without closing (power loss, network cut): without this the
		 * kernel would take hours to notice. About 25 seconds with these values. */
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
		{
			int idle = 10, interval = 5, count = 3;
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
		}
#endif
	}
	return fd;
}

int as_connect_timeout(const char *host, int port, int timeout_ms)
{
	return as_connect_abortable(host, port, timeout_ms, NULL, NULL);
}

int as_send_all(int fd, const uint8_t *buf, size_t n, int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;
	size_t off = 0;

	while (off < n) {
		ssize_t k = send(fd, buf + off, n - off, AS_SEND_FLAGS);
		if (k > 0) {
			off += (size_t)k;
			continue;
		}
		if (k < 0 && errno == EINTR) {
			continue;
		}
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			long long left = deadline - now_ms();
			if (left <= 0 || wait_fd(fd, POLLOUT, (int)left) <= 0) {
				return -1;
			}
			continue;
		}
		return -1;
	}
	return 0;
}
