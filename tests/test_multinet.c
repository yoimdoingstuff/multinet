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
    mn_instance *scanner = NULL;
    mn_room discovered_rooms[MN_MAX_ROOMS];
    size_t discovered_count = 0;
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
    scanner = mn_create(&config);
    CHECK(host != NULL && client != NULL && scanner != NULL,
          "create independent host, client and discovery instances");
    CHECK(mn_host(host, "Host", "Local Test", MN_MAX_PLAYERS), "host local room");

    CHECK(!mn_discover(scanner),
          "disabled broadcast reports that an alternate discovery path is required");
    CHECK(mn_discover_at(scanner, "127.0.0.1", config.port),
          "start direct unicast discovery when broadcast is disabled");
    for (iteration = 0; iteration < 40; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(scanner, 0.01f);
        discovered_count = mn_get_rooms(scanner, discovered_rooms, MN_MAX_ROOMS);
        if (discovered_count > 0) break;
        sleep_ms(5);
    }
    CHECK(discovered_count == 1 &&
          strcmp(discovered_rooms[0].name, "Local Test") == 0,
          "unicast discovery finds room without LAN broadcast");

    CHECK(mn_join(client, "127.0.0.1", config.port, "Client"),
          "manual IPv4 join works as final discovery fallback");

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


    mn_get_info(client, &client_info);
    CHECK(client_info.match_started != 0, "client receives match-start event");

    /* Fill the room to its advertised 16-player cap, using separate instances
       in this process to exercise independent client sockets and host slots. */
    for (j = 0; j < MN_MAX_PLAYERS - 2u; ++j) {
        char name[MN_PLAYER_NAME_LENGTH];
        extra_clients[j] = mn_create(&config);
        CHECK(extra_clients[j] != NULL, "create additional player instance");
        snprintf(name, sizeof(name), "Guest%02u", (unsigned)(j + 1u));
        if (j == 0) {
            CHECK(mn_join_room(extra_clients[j], &discovered_rooms[0], name),
                  "join a discovered room through the room convenience API");
        } else {
            CHECK(mn_join(extra_clients[j], "127.0.0.1", config.port, name),
                  "additional client starts joining by manual IPv4 address");
        }
        extra_count++;
    }
    for (iteration = 0; iteration < 250; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        for (j = 0; j < extra_count; ++j) mn_update(extra_clients[j], 0.01f);
        sleep_ms(4);
    }
    mn_get_info(host, &host_info);
    CHECK(host_info.player_count == MN_MAX_PLAYERS,
          "host supports 16 simultaneous players including the host");
    for (j = 0; j < extra_count; ++j) {
        mn_get_info(extra_clients[j], &client_info);
        CHECK(client_info.status == MN_STATUS_CONNECTED,
              "all additional clients connect");
    }

    for (j = 0; j < extra_count; ++j) mn_leave(extra_clients[j]);
    mn_leave(client);
    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(host, 0.01f);
        sleep_ms(5);
    }
    mn_get_info(host, &host_info);
    CHECK(host_info.player_count == 1, "host removes players who leave");

    printf("MultiNet tests passed (16-player room, snapshots, messages, match start, leave).\n");
cleanup:
    for (j = 0; j < extra_count; ++j) if (extra_clients[j]) mn_destroy(extra_clients[j]);
    if (scanner) mn_destroy(scanner);
    if (client) mn_destroy(client);
    if (host) mn_destroy(host);
    return result;
}
