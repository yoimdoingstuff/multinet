/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet_stream.h"
#include "multinet_tcp.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned int milliseconds) { Sleep(milliseconds); }
#else
#include <time.h>
static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    (void)nanosleep(&delay, NULL);
}
#endif

static int failures;

#define CHECK(condition, description) do { \
    if (condition) printf("[PASS] %s\n", description); \
    else { fprintf(stderr, "[FAIL] %s\n", description); failures++; } \
} while (0)

static int wait_connected(mn_tcp *host, mn_tcp *client) {
    unsigned i;
    mn_tcp_info host_info, client_info;
    for (i = 0; i < 400u; ++i) {
        if (!mn_tcp_update(host) || !mn_tcp_update(client)) return 0;
        mn_tcp_get_info(host, &host_info);
        mn_tcp_get_info(client, &client_info);
        if (host_info.state == MN_TCP_CONNECTED &&
            client_info.state == MN_TCP_CONNECTED)
            return 1;
        sleep_ms(5u);
    }
    return 0;
}

static int wait_for_message(mn_stream *receiver, mn_stream *sender,
                            uint16_t expected_channel, const char *expected_text) {
    char buffer[128];
    size_t received = 0;
    uint16_t channel = 0;
    unsigned i;
    for (i = 0; i < 400u; ++i) {
        if (!mn_stream_update(sender) || !mn_stream_update(receiver)) return 0;
        if (mn_stream_poll(receiver, &channel, buffer, sizeof(buffer) - 1u,
                           &received) == 1) {
            buffer[received] = '\0';
            return channel == expected_channel &&
                   received == strlen(expected_text) &&
                   memcmp(buffer, expected_text, received) == 0;
        }
        sleep_ms(5u);
    }
    return 0;
}

int main(void) {
    mn_tcp *host = NULL;
    mn_tcp *client = NULL;
    mn_tcp *replacement = NULL;
    mn_tcp_info info;
    mn_stream_transport host_transport, client_transport, replacement_transport;
    mn_stream *host_stream = NULL;
    mn_stream *client_stream = NULL;
    mn_stream *replacement_stream = NULL;
    uint16_t port = 0;
    unsigned i;
    int host_stream_failed = 0;

    CHECK(mn_tcp_supported(), "TCP transport is available on this platform");
    host = mn_tcp_create();
    client = mn_tcp_create();
    CHECK(host != NULL && client != NULL, "create independent TCP endpoints");
    if (!host || !client) goto cleanup;

    CHECK(mn_tcp_host(host, "127.0.0.1", 0u),
          "host a direct TCP listener on an OS-assigned loopback port");
    mn_tcp_get_info(host, &info);
    port = info.local_port;
    CHECK(port != 0u && info.state == MN_TCP_LISTENING,
          "read back the ephemeral listening port");
    CHECK(port != 0u && mn_tcp_join(client, "127.0.0.1", port),
          "start a non-blocking direct TCP client connection");
    CHECK(wait_connected(host, client),
          "complete TCP host/client connection without blocking");

    CHECK(mn_stream_transport_from_tcp(host, &host_transport) &&
          mn_stream_transport_from_tcp(client, &client_transport),
          "adapt TCP sockets to the framed-message transport interface");
    host_stream = mn_stream_create(&host_transport, 256u);
    client_stream = mn_stream_create(&client_transport, 256u);
    CHECK(host_stream != NULL && client_stream != NULL,
          "create framed message channels over TCP");
    if (!host_stream || !client_stream) goto cleanup;

    CHECK(mn_stream_send(client_stream, 42u, "TCP client to host", 18u),
          "queue a client-to-host framed message");
    CHECK(mn_stream_send(host_stream, 43u, "TCP host to client", 18u),
          "queue a host-to-client framed message");
    CHECK(wait_for_message(host_stream, client_stream, 42u,
                           "TCP client to host"),
          "receive the client message over TCP");
    CHECK(wait_for_message(client_stream, host_stream, 43u,
                           "TCP host to client"),
          "receive the host message over TCP");

    mn_tcp_close(client);
    for (i = 0; i < 400u; ++i) {
        if (!mn_stream_update(host_stream)) {
            host_stream_failed = 1;
            break;
        }
        sleep_ms(5u);
    }
    mn_tcp_get_info(host, &info);
    CHECK(host_stream_failed && info.state == MN_TCP_LISTENING,
          "detect a disconnected client while preserving the TCP host listener");

    replacement = mn_tcp_create();
    CHECK(replacement != NULL &&
          mn_tcp_join(replacement, "127.0.0.1", port),
          "allow a replacement TCP client to join the existing listener");
    if (!replacement) goto cleanup;
    for (i = 0; i < 400u; ++i) {
        if (!mn_tcp_update(host) || !mn_tcp_update(replacement)) break;
        mn_tcp_get_info(host, &info);
        {
            mn_tcp_info replacement_info;
            mn_tcp_get_info(replacement, &replacement_info);
            if (info.state == MN_TCP_CONNECTED &&
                replacement_info.state == MN_TCP_CONNECTED)
                break;
        }
        sleep_ms(5u);
    }
    mn_tcp_get_info(host, &info);
    CHECK(info.state == MN_TCP_CONNECTED,
          "accept the replacement client on the original host listener");
    mn_stream_reset(host_stream);
    CHECK(mn_stream_transport_from_tcp(replacement, &replacement_transport),
          "adapt the replacement TCP client");
    replacement_stream = mn_stream_create(&replacement_transport, 256u);
    CHECK(replacement_stream != NULL,
          "create a fresh framed client stream");
    if (!replacement_stream) goto cleanup;
    CHECK(mn_stream_send(replacement_stream, 44u, "after reconnect", 15u),
          "queue a message in the replacement TCP session");
    CHECK(wait_for_message(host_stream, replacement_stream, 44u,
                           "after reconnect"),
          "resume framed messaging after a TCP peer replacement");

cleanup:
    if (replacement_stream) mn_stream_destroy(replacement_stream);
    if (client_stream) mn_stream_destroy(client_stream);
    if (host_stream) mn_stream_destroy(host_stream);
    if (replacement) mn_tcp_destroy(replacement);
    if (client) mn_tcp_destroy(client);
    if (host) mn_tcp_destroy(host);
    if (failures != 0) {
        fprintf(stderr, "%d TCP transport test(s) failed.\n", failures);
        return 1;
    }
    puts("MultiNet TCP transport tests passed.");
    return 0;
}
