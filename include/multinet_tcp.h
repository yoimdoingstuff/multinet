/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#ifndef MULTINET_TCP_H
#define MULTINET_TCP_H

#include <stddef.h>
#include <stdint.h>
#include "multinet_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mn_tcp mn_tcp;

typedef enum mn_tcp_state {
    MN_TCP_IDLE = 0,
    MN_TCP_LISTENING = 1,
    MN_TCP_CONNECTING = 2,
    MN_TCP_CONNECTED = 3
} mn_tcp_state;

typedef struct mn_tcp_info {
    mn_tcp_state state;
    uint16_t local_port;
    char remote_address[48];
    char last_error[160];
} mn_tcp_info;

/* TCP is a direct, single-peer byte stream. It is supported on Windows and
   POSIX platforms. It does not create a relay or bypass firewall policy. */
int mn_tcp_supported(void);
mn_tcp *mn_tcp_create(void);
void mn_tcp_destroy(mn_tcp *connection);

/* Listen for one peer on an IPv4 address. Pass NULL to listen on all local
   IPv4 interfaces. Port zero asks the OS to assign an ephemeral port; read
   the selected value from mn_tcp_info.local_port. The listener remains open
   after a peer disconnects so another client can join. */
int mn_tcp_host(mn_tcp *connection, const char *local_ipv4, uint16_t port);

/* Connect to an IPv4 address or resolvable hostname and TCP port. The socket
   is non-blocking; call mn_tcp_update() until state becomes CONNECTED. */
int mn_tcp_join(mn_tcp *connection, const char *host, uint16_t port);

/* Poll accept/connect progress. Call from the owning thread. */
int mn_tcp_update(mn_tcp *connection);

/* Non-blocking stream I/O. Calls may transfer fewer bytes than requested;
   a successful call with *out_transferred == 0 means would-block. A return
   value of zero means the peer disconnected or an I/O error occurred. */
int mn_tcp_read(mn_tcp *connection, void *buffer, size_t capacity,
                size_t *out_transferred);
int mn_tcp_write(mn_tcp *connection, const void *data, size_t size,
                 size_t *out_transferred);

void mn_tcp_close(mn_tcp *connection);
void mn_tcp_get_info(const mn_tcp *connection, mn_tcp_info *out_info);

/* Create a caller-owned, non-blocking stream adapter for mn_stream. */
int mn_stream_transport_from_tcp(mn_tcp *connection,
                                 mn_stream_transport *out_transport);

#ifdef __cplusplus
}
#endif
#endif /* MULTINET_TCP_H */
