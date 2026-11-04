/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
/* Direct IPv4 TCP stream transport for MultiNet's framed-message layer. */
#include "multinet_tcp.h"
#include "multinet_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET mn_tcp_socket;
typedef int mn_tcp_socklen;
#define MN_TCP_BAD_SOCKET INVALID_SOCKET
#define mn_tcp_close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int mn_tcp_socket;
typedef socklen_t mn_tcp_socklen;
#define MN_TCP_BAD_SOCKET (-1)
#define mn_tcp_close_socket close
#endif

struct mn_tcp {
    mn_tcp_socket listener;
    mn_tcp_socket socket;
    int winsock_started;
    mn_tcp_info info;
};

static void mn_tcp_set_error(mn_tcp *connection, const char *message) {
    if (!connection) return;
    (void)snprintf(connection->info.last_error,
                   sizeof(connection->info.last_error), "%s",
                   message ? message : "TCP transport error");
}

#ifdef _WIN32
static void mn_tcp_set_os_error(mn_tcp *connection,
                                const char *operation, int error_code) {
    char message[160];
    (void)snprintf(message, sizeof(message), "%s failed (Winsock error %d)",
                   operation, error_code);
    mn_tcp_set_error(connection, message);
}

static int mn_tcp_would_block(int error_code) {
    return error_code == WSAEWOULDBLOCK || error_code == WSAEINPROGRESS ||
           error_code == WSAEALREADY;
}
#else
static void mn_tcp_set_os_error(mn_tcp *connection,
                                const char *operation, int error_code) {
    char message[160];
    (void)snprintf(message, sizeof(message), "%s failed (%s)",
                   operation, strerror(error_code));
    mn_tcp_set_error(connection, message);
}

static int mn_tcp_would_block(int error_code) {
    return error_code == EWOULDBLOCK || error_code == EAGAIN ||
           error_code == EINPROGRESS || error_code == EALREADY;
}
#endif

static int mn_tcp_set_nonblocking(mn_tcp_socket socket_value) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket_value, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(socket_value, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static void mn_tcp_close_handle(mn_tcp_socket *socket_value) {
    if (socket_value && *socket_value != MN_TCP_BAD_SOCKET) {
        mn_tcp_close_socket(*socket_value);
        *socket_value = MN_TCP_BAD_SOCKET;
    }
}

static void mn_tcp_copy_remote(mn_tcp *connection,
                               const struct sockaddr_in *remote) {
    if (!connection || !remote) return;
    if (!inet_ntop(AF_INET, &remote->sin_addr,
                   connection->info.remote_address,
                   sizeof(connection->info.remote_address)))
        connection->info.remote_address[0] = '\0';
}

static int mn_tcp_open_socket(mn_tcp *connection, mn_tcp_socket *out_socket) {
    mn_tcp_socket socket_value;
    if (!connection || !out_socket) return 0;
    socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_value == MN_TCP_BAD_SOCKET) {
#ifdef _WIN32
        mn_tcp_set_os_error(connection, "TCP socket", WSAGetLastError());
#else
        mn_tcp_set_os_error(connection, "TCP socket", errno);
#endif
        return 0;
    }
    if (!mn_tcp_set_nonblocking(socket_value)) {
#ifdef _WIN32
        mn_tcp_set_os_error(connection, "TCP non-blocking mode", WSAGetLastError());
#else
        mn_tcp_set_os_error(connection, "TCP non-blocking mode", errno);
#endif
        mn_tcp_close_socket(socket_value);
        return 0;
    }
#ifndef _WIN32
#ifdef SO_NOSIGPIPE
    {
        int no_sigpipe = 1;
        (void)setsockopt(socket_value, SOL_SOCKET, SO_NOSIGPIPE,
                         &no_sigpipe, (socklen_t)sizeof(no_sigpipe));
    }
#endif
#endif
    *out_socket = socket_value;
    return 1;
}

static void mn_tcp_drop_peer(mn_tcp *connection) {
    if (!connection) return;
    mn_tcp_close_handle(&connection->socket);
    connection->info.remote_address[0] = '\0';
    connection->info.state = connection->listener != MN_TCP_BAD_SOCKET
        ? MN_TCP_LISTENING : MN_TCP_IDLE;
}

int mn_tcp_supported(void) {
#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
    return 1;
#else
    return 0;
#endif
}

mn_tcp *mn_tcp_create(void) {
    mn_tcp *connection = (mn_tcp *)calloc(1u, sizeof(*connection));
    if (!connection) return NULL;
    connection->listener = MN_TCP_BAD_SOCKET;
    connection->socket = MN_TCP_BAD_SOCKET;
#ifdef _WIN32
    {
        WSADATA data;
        int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            free(connection);
            return NULL;
        }
        connection->winsock_started = 1;
    }
#endif
    return connection;
}

void mn_tcp_close(mn_tcp *connection) {
    if (!connection) return;
    mn_tcp_close_handle(&connection->socket);
    mn_tcp_close_handle(&connection->listener);
    memset(&connection->info, 0, sizeof(connection->info));
    connection->info.state = MN_TCP_IDLE;
}

void mn_tcp_destroy(mn_tcp *connection) {
    if (!connection) return;
    mn_tcp_close(connection);
#ifdef _WIN32
    if (connection->winsock_started) WSACleanup();
#endif
    free(connection);
}

int mn_tcp_host(mn_tcp *connection, const char *local_ipv4, uint16_t port) {
    mn_tcp_socket listener = MN_TCP_BAD_SOCKET;
    struct sockaddr_in local;
#ifndef _WIN32
    int reuse = 1;
#endif
    if (!connection) return 0;
    mn_tcp_close(connection);
    if (!mn_tcp_open_socket(connection, &listener)) return 0;
#ifdef _WIN32
    {
        int exclusive = 1;
        (void)setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         (const char *)&exclusive, (int)sizeof(exclusive));
    }
#else
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     &reuse, (socklen_t)sizeof(reuse));
#endif
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (local_ipv4 && local_ipv4[0]) {
        if (inet_pton(AF_INET, local_ipv4, &local.sin_addr) != 1) {
            mn_tcp_set_error(connection, "The selected TCP host address is not valid IPv4");
            mn_tcp_close_socket(listener);
            return 0;
        }
    } else {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(listener, (struct sockaddr *)&local,
             (mn_tcp_socklen)sizeof(local)) != 0) {
#ifdef _WIN32
        mn_tcp_set_os_error(connection, "TCP bind", WSAGetLastError());
#else
        mn_tcp_set_os_error(connection, "TCP bind", errno);
#endif
        mn_tcp_close_socket(listener);
        return 0;
    }
    if (listen(listener, SOMAXCONN) != 0) {
#ifdef _WIN32
        mn_tcp_set_os_error(connection, "TCP listen", WSAGetLastError());
#else
        mn_tcp_set_os_error(connection, "TCP listen", errno);
#endif
        mn_tcp_close_socket(listener);
        return 0;
    }
    {
        mn_tcp_socklen address_length = (mn_tcp_socklen)sizeof(local);
        if (getsockname(listener, (struct sockaddr *)&local,
                        &address_length) != 0) {
#ifdef _WIN32
            mn_tcp_set_os_error(connection, "TCP local port lookup", WSAGetLastError());
#else
            mn_tcp_set_os_error(connection, "TCP local port lookup", errno);
#endif
            mn_tcp_close_socket(listener);
            return 0;
        }
    }
    connection->listener = listener;
    connection->info.local_port = ntohs(local.sin_port);
    connection->info.state = MN_TCP_LISTENING;
    connection->info.last_error[0] = '\0';
    return 1;
}

int mn_tcp_join(mn_tcp *connection, const char *host, uint16_t port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct sockaddr_in remote;
    mn_tcp_socket socket_value = MN_TCP_BAD_SOCKET;
    int resolve_result;
    int connect_result;
    if (!connection || !host || !host[0] || port == 0u) {
        if (connection) mn_tcp_set_error(connection, "TCP host and non-zero port are required");
        return 0;
    }
    mn_tcp_close(connection);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    resolve_result = getaddrinfo(host, NULL, &hints, &result);
    if (resolve_result != 0 || !result) {
        mn_tcp_set_error(connection, "Could not resolve the TCP host as IPv4");
        if (result) freeaddrinfo(result);
        return 0;
    }
    memset(&remote, 0, sizeof(remote));
    memcpy(&remote, result->ai_addr, sizeof(remote));
    remote.sin_port = htons(port);
    freeaddrinfo(result);
    if (!mn_tcp_open_socket(connection, &socket_value)) return 0;
    connect_result = connect(socket_value, (struct sockaddr *)&remote,
                             (mn_tcp_socklen)sizeof(remote));
    if (connect_result != 0) {
#ifdef _WIN32
        int error_code = WSAGetLastError();
#else
        int error_code = errno;
#endif
        if (!mn_tcp_would_block(error_code)) {
            mn_tcp_set_os_error(connection, "TCP connect", error_code);
            mn_tcp_close_socket(socket_value);
            return 0;
        }
        connection->info.state = MN_TCP_CONNECTING;
    } else {
        connection->info.state = MN_TCP_CONNECTED;
    }
    connection->socket = socket_value;
    mn_tcp_copy_remote(connection, &remote);
    connection->info.last_error[0] = '\0';
    {
        struct sockaddr_in local;
        mn_tcp_socklen local_length = (mn_tcp_socklen)sizeof(local);
        memset(&local, 0, sizeof(local));
        if (getsockname(socket_value, (struct sockaddr *)&local,
                        &local_length) == 0)
            connection->info.local_port = ntohs(local.sin_port);
    }
    return 1;
}

int mn_tcp_update(mn_tcp *connection) {
    if (!connection) return 0;
    if (connection->listener != MN_TCP_BAD_SOCKET &&
        connection->socket == MN_TCP_BAD_SOCKET) {
        struct sockaddr_in remote;
        mn_tcp_socklen remote_length = (mn_tcp_socklen)sizeof(remote);
        mn_tcp_socket accepted;
        memset(&remote, 0, sizeof(remote));
        accepted = accept(connection->listener, (struct sockaddr *)&remote,
                          &remote_length);
        if (accepted != MN_TCP_BAD_SOCKET) {
            if (!mn_tcp_set_nonblocking(accepted)) {
#ifdef _WIN32
                mn_tcp_set_os_error(connection, "Accepted TCP non-blocking mode",
                                    WSAGetLastError());
#else
                mn_tcp_set_os_error(connection, "Accepted TCP non-blocking mode",
                                    errno);
#endif
                mn_tcp_close_socket(accepted);
                return 0;
            }
            connection->socket = accepted;
            connection->info.state = MN_TCP_CONNECTED;
            connection->info.last_error[0] = '\0';
            mn_tcp_copy_remote(connection, &remote);
        } else {
#ifdef _WIN32
            int error_code = WSAGetLastError();
#else
            int error_code = errno;
#endif
            if (!mn_tcp_would_block(error_code)) {
                mn_tcp_set_os_error(connection, "TCP accept", error_code);
                return 0;
            }
        }
    }

    if (connection->info.state == MN_TCP_CONNECTING &&
        connection->socket != MN_TCP_BAD_SOCKET) {
        fd_set write_set, error_set;
        struct timeval timeout;
        int select_result;
        int socket_error = 0;
        mn_tcp_socklen error_length = (mn_tcp_socklen)sizeof(socket_error);
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
        FD_SET(connection->socket, &write_set);
        FD_SET(connection->socket, &error_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
#ifdef _WIN32
        select_result = select(0, NULL, &write_set, &error_set, &timeout);
#else
        select_result = select(connection->socket + 1, NULL, &write_set,
                                &error_set, &timeout);
#endif
        if (select_result == -1) {
#ifdef _WIN32
            mn_tcp_set_os_error(connection, "TCP connection poll", WSAGetLastError());
#else
            mn_tcp_set_os_error(connection, "TCP connection poll", errno);
#endif
            mn_tcp_drop_peer(connection);
            return 0;
        }
        if (select_result > 0 &&
            (FD_ISSET(connection->socket, &write_set) ||
             FD_ISSET(connection->socket, &error_set))) {
            if (getsockopt(connection->socket, SOL_SOCKET, SO_ERROR,
                           (char *)&socket_error, &error_length) != 0) {
#ifdef _WIN32
                socket_error = WSAGetLastError();
#else
                socket_error = errno;
#endif
            }
            if (socket_error != 0) {
                mn_tcp_set_os_error(connection, "TCP connect",
                                    socket_error);
                mn_tcp_drop_peer(connection);
                return 0;
            }
            connection->info.state = MN_TCP_CONNECTED;
            connection->info.last_error[0] = '\0';
        }
    }
    return 1;
}

int mn_tcp_read(mn_tcp *connection, void *buffer, size_t capacity,
                size_t *out_transferred) {
    int received;
    int request;
    if (out_transferred) *out_transferred = 0;
    if (!connection || !buffer || capacity == 0u ||
        connection->info.state != MN_TCP_CONNECTED ||
        connection->socket == MN_TCP_BAD_SOCKET)
        return 0;
    request = capacity > (size_t)INT_MAX ? INT_MAX : (int)capacity;
    received = recv(connection->socket, (char *)buffer, request, 0);
    if (received > 0) {
        if (out_transferred) *out_transferred = (size_t)received;
        return 1;
    }
    if (received == 0) {
        mn_tcp_drop_peer(connection);
        mn_tcp_set_error(connection, "TCP peer disconnected");
        return 0;
    }
#ifdef _WIN32
    {
        int error_code = WSAGetLastError();
        if (mn_tcp_would_block(error_code)) return 1;
        mn_tcp_set_os_error(connection, "TCP read", error_code);
    }
#else
    {
        int error_code = errno;
        if (mn_tcp_would_block(error_code)) return 1;
        mn_tcp_set_os_error(connection, "TCP read", error_code);
    }
#endif
    mn_tcp_drop_peer(connection);
    return 0;
}

int mn_tcp_write(mn_tcp *connection, const void *data, size_t size,
                 size_t *out_transferred) {
    int sent;
    int request;
    int flags = 0;
    if (out_transferred) *out_transferred = 0;
    if (!connection || (!data && size != 0u) ||
        connection->info.state != MN_TCP_CONNECTED ||
        connection->socket == MN_TCP_BAD_SOCKET)
        return 0;
    if (size == 0u) return 1;
    request = size > (size_t)INT_MAX ? INT_MAX : (int)size;
#ifndef _WIN32
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
#endif
    sent = send(connection->socket, (const char *)data, request, flags);
    if (sent > 0) {
        if (out_transferred) *out_transferred = (size_t)sent;
        return 1;
    }
    if (sent == 0) {
        mn_tcp_drop_peer(connection);
        mn_tcp_set_error(connection, "TCP peer disconnected during write");
        return 0;
    }
#ifdef _WIN32
    {
        int error_code = WSAGetLastError();
        if (mn_tcp_would_block(error_code)) return 1;
        mn_tcp_set_os_error(connection, "TCP write", error_code);
    }
#else
    {
        int error_code = errno;
        if (mn_tcp_would_block(error_code)) return 1;
        mn_tcp_set_os_error(connection, "TCP write", error_code);
    }
#endif
    mn_tcp_drop_peer(connection);
    return 0;
}

void mn_tcp_get_info(const mn_tcp *connection, mn_tcp_info *out_info) {
    if (!out_info) return;
    memset(out_info, 0, sizeof(*out_info));
    if (connection) *out_info = connection->info;
    else (void)snprintf(out_info->last_error, sizeof(out_info->last_error),
                        "Invalid TCP instance");
}

static int mn_tcp_transport_update(void *context) {
    return mn_tcp_update((mn_tcp *)context);
}

static int mn_tcp_transport_connected(void *context) {
    mn_tcp_info info;
    mn_tcp_get_info((const mn_tcp *)context, &info);
    return info.state == MN_TCP_CONNECTED;
}

static int mn_tcp_transport_read(void *context, void *buffer,
                                 size_t capacity, size_t *out_transferred) {
    return mn_tcp_read((mn_tcp *)context, buffer, capacity, out_transferred);
}

static int mn_tcp_transport_write(void *context, const void *data,
                                  size_t size, size_t *out_transferred) {
    return mn_tcp_write((mn_tcp *)context, data, size, out_transferred);
}

int mn_stream_transport_from_tcp(mn_tcp *connection,
                                 mn_stream_transport *out_transport) {
    if (!connection || !out_transport) return 0;
    memset(out_transport, 0, sizeof(*out_transport));
    out_transport->context = connection;
    out_transport->update = mn_tcp_transport_update;
    out_transport->is_connected = mn_tcp_transport_connected;
    out_transport->read = mn_tcp_transport_read;
    out_transport->write = mn_tcp_transport_write;
    return 1;
}
