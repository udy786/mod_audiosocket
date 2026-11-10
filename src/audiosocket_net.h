/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2026 Uday Kumar
 *
 * Small TCP helpers for the AudioSocket client: address parsing, connect with a
 * timeout, and send-all. POSIX sockets, no FreeSWITCH dependency.
 *
 * NOTE: as_connect_timeout resolves the host name with getaddrinfo, which can block
 * for longer than timeout_ms if DNS is slow. It must be called from the call's own
 * application thread, never from a media or IO thread. Use an IP address to avoid DNS.
 */
#ifndef AUDIOSOCKET_NET_H
#define AUDIOSOCKET_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Split "host:port", "1.2.3.4:9000" or "[::1]:9000" into host (copied into host, NUL terminated)
 * and port (1 to 65535). Returns 0 on success, -1 if malformed or host does not fit. */
int as_parse_hostport(const char *s, char *host, size_t host_cap, int *port);

/* Connect a TCP socket within timeout_ms (total, across all resolved addresses).
 * Returns a non-blocking file descriptor, or -1 on failure. The descriptor is close-on-exec, has
 * TCP_NODELAY set and TCP keepalive on (a vanished server is noticed after about 25 seconds).
 * A literal IP address is never looked up. */
int as_connect_timeout(const char *host, int port, int timeout_ms);

/* The same, but while waiting for the connection should_abort(ctx) is asked every 100 ms and the connect
 * is given up (-1) when it returns non-zero, so a caller that has hung up is not left waiting.
 * Only the wait for the connection is covered: a host name lookup cannot be interrupted. */
int as_connect_abortable(const char *host, int port, int timeout_ms, int (*should_abort)(void *), void *ctx);

/* Send all n bytes on a non-blocking socket, waiting up to timeout_ms in total.
 * Returns 0 on success, -1 on error, timeout or closed connection. Never raises SIGPIPE. */
int as_send_all(int fd, const uint8_t *buf, size_t n, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AUDIOSOCKET_NET_H */
