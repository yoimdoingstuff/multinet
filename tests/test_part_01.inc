/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned int ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(unsigned int ms) {
    struct timespec delay;
    delay.tv_sec = (time_t)(ms / 1000u);
    delay.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}
#endif

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        result = 1; goto cleanup; } \
} while (0)

int main(void) {
    mn_config config;
    mn_instance *host = NULL;
    mn_instance *client = NULL;
    mn_instance *extra_clients[MN_MAX_PLAYERS - 2u] = {0};
    mn_info host_info, client_info;
    mn_player state, received;
    mn_event event;
    int result = 0;
    int connected = 0, message_received = 0, host_message_received = 0;
    unsigned iteration;
    size_t extra_count = 0, j;

    mn_config_init(&config);
    config.port = 38927;
    config.enable_broadcast = 0;
    host = mn_create(&config);
    client = mn_create(&config);
    CHECK(host != NULL && client != NULL, "create two independent instances");
    CHECK(mn_host(host, "Host", "Local Test", MN_MAX_PLAYERS), "host local room");
    CHECK(mn_join(client, "127.0.0.1", config.port, "Client"), "start client join");

    memset(&state, 0, sizeof(state));
    state.position.x = 12.5f;
    state.position.y = -3.25f;
    state.rotation.w = 1.0f;
    state.velocity.x = 1.0f;
    state.colour_rgba = 0xFF8844FFu;
    state.score = 7;
    state.appearance[0] = 12;

    for (iteration = 0; iteration < 500; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        mn_get_info(client, &client_info);
        if (client_info.status == MN_STATUS_CONNECTED) { connected = 1; break; }
        sleep_ms(5);
    }
    CHECK(connected, "client receives welcome packet");

    for (iteration = 0; iteration < 80; ++iteration) {
        (void)mn_set_local_player(client, &state);
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        sleep_ms(5);
    }
    mn_get_info(host, &host_info);
    mn_get_info(client, &client_info);
    CHECK(host_info.player_count == 2 && client_info.player_count == 2,
          "both instances receive the two-player roster");
    CHECK(mn_get_player(host, client_info.local_player_id, &received),
          "host can look up remote player by ID");
    CHECK(received.position.x == 12.5f && received.position.y == -3.25f &&
          received.score == 7 && received.appearance[0] == 12,
          "player snapshot fields survive wire serialisation");

    CHECK(mn_send_message(client, 0, 42, "client-to-host", 14),
          "client sends an application message");
    for (iteration = 0; iteration < 20 && !message_received; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        while (mn_poll_event(host, &event)) {
            if (event.type == MN_EVENT_MESSAGE && event.channel == 42 &&
                event.data_size == 14 && memcmp(event.data, "client-to-host", 14) == 0)
                message_received = 1;
        }
        sleep_ms(5);
    }
    CHECK(message_received, "host receives client application message");

    mn_get_info(client, &client_info);
    CHECK(mn_send_message(host, client_info.local_player_id, 77, "host-reply", 10),
          "host sends a targeted application message");
    for (iteration = 0; iteration < 20 && !host_message_received; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        while (mn_poll_event(client, &event)) {
            if (event.type == MN_EVENT_MESSAGE && event.channel == 77 &&
                event.data_size == 10 && memcmp(event.data, "host-reply", 10) == 0)
                host_message_received = 1;
        }
        sleep_ms(5);
    }
    CHECK(host_message_received, "client receives targeted host message");

    CHECK(mn_start_match(host), "host starts match");
    for (iteration = 0; iteration < 10; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        sleep_ms(5);
    }
