/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet.h"

#include <stdio.h>
#include <stdlib.h>
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

static int parse_port(const char *text, uint16_t *out_port) {
    char *end = NULL;
    unsigned long value;
    if (!text || !out_port || !text[0]) return 0;
    value = strtoul(text, &end, 10);
    if (!end || *end != '\0' || value == 0ul || value > 65535ul) return 0;
    *out_port = (uint16_t)value;
    return 1;
}

static void print_events(mn_instance *network, uint32_t delivery_id,
                         int *acknowledged, int *failed) {
    mn_event event;
    while (mn_poll_event(network, &event)) {
        switch (event.type) {
            case MN_EVENT_PLAYER_JOINED:
                printf("Player %u joined: %s\n", (unsigned)event.player_id,
                       event.player.name);
                break;
            case MN_EVENT_PLAYER_LEFT:
                printf("Player %u left the room\n", (unsigned)event.player_id);
                break;
            case MN_EVENT_MESSAGE:
                printf("Message from player %u: %.*s\n",
                       (unsigned)event.player_id, (int)event.data_size,
                       (const char *)event.data);
                break;
            case MN_EVENT_MESSAGE_ACKNOWLEDGED:
                if (event.delivery_id == delivery_id && acknowledged)
                    *acknowledged = 1;
                printf("Delivery %u acknowledged by player %u\n",
                       (unsigned)event.delivery_id, (unsigned)event.player_id);
                break;
            case MN_EVENT_MESSAGE_FAILED:
                if (event.delivery_id == delivery_id && failed)
                    *failed = 1;
                printf("Delivery %u failed for player %u: %s\n",
                       (unsigned)event.delivery_id, (unsigned)event.player_id,
                       event.text);
                break;
            case MN_EVENT_ERROR:
                fprintf(stderr, "MultiNet: %s\n", event.text);
                break;
            default:
                break;
        }
    }
}

int main(int argc, char **argv) {
    mn_config config;
    mn_instance *network = NULL;
    mn_info info;
    const char *mode;
    const char *name;
    const char *message;
    uint16_t port = MN_DEFAULT_PORT;
    uint32_t delivery_id = 0;
    unsigned iteration;
    int is_host;
    int acknowledged = 0;
    int failed = 0;
    int result = 1;

    if (argc < 4 || argc > 6) {
        fprintf(stderr,
            "Usage:\n"
            "  %s host <name> <message> [port]\n"
            "  %s join <host-ip> <name> <message> [port]\n",
            argv[0], argv[0]);
#ifdef _WIN32
        if (argc == 1) {
            MessageBoxA(NULL,
                "This is a command-line chat example. Run it from PowerShell with "
                "'host' or 'join' arguments. Open multinet_demo.exe for the graphical multiplayer demo.",
                "MultiNet chat example", MB_OK | MB_ICONINFORMATION);
        }
#endif
        return 2;
    }
    mode = argv[1];
    is_host = strcmp(mode, "host") == 0;
    if (!is_host && strcmp(mode, "join") != 0) {
        fprintf(stderr, "Mode must be 'host' or 'join'.\n");
        return 2;
    }
    if (is_host) {
        if (argc < 4 || argc > 5) return 2;
        name = argv[2];
        message = argv[3];
        if (argc == 5 && !parse_port(argv[4], &port)) {
            fprintf(stderr, "Invalid UDP port.\n");
            return 2;
        }
    } else {
        if (argc < 5 || argc > 6) return 2;
        name = argv[3];
        message = argv[4];
        if (argc == 6 && !parse_port(argv[5], &port)) {
            fprintf(stderr, "Invalid UDP port.\n");
            return 2;
        }
    }
    if (strlen(message) > MN_MAX_MESSAGE_BYTES) {
        fprintf(stderr, "Message exceeds %u bytes.\n",
                (unsigned)MN_MAX_MESSAGE_BYTES);
        return 2;
    }

    mn_config_init(&config);
    config.port = port;
    network = mn_create(&config);
    if (!network) {
        fprintf(stderr, "Could not create MultiNet instance.\n");
        return 1;
    }
    if (is_host) {
        if (!mn_host(network, name, "MultiNet Chat", MN_MAX_PLAYERS)) {
            mn_get_info(network, &info);
            fprintf(stderr, "Could not host: %s\n", info.last_error);
            goto cleanup;
        }
        printf("Hosting chat on UDP %u. Waiting for a client...\n",
               (unsigned)port);
    } else {
        if (!mn_join(network, argv[2], port, name)) {
            mn_get_info(network, &info);
            fprintf(stderr, "Could not start join: %s\n", info.last_error);
            goto cleanup;
        }
        printf("Joining %s:%u...\n", argv[2], (unsigned)port);
    }

    for (iteration = 0; iteration < 3000u; ++iteration) {
        mn_update(network, 0.01f);
        print_events(network, 0u, NULL, NULL);
        mn_get_info(network, &info);
        if (is_host ? info.player_count > 1u :
                      info.status == MN_STATUS_CONNECTED)
            break;
        if (!is_host && info.status == MN_STATUS_OFFLINE) {
            fprintf(stderr, "Join failed or timed out: %s\n", info.last_error);
            goto cleanup;
        }
        sleep_ms(10u);
    }
    mn_get_info(network, &info);
    if (is_host ? info.player_count <= 1u :
                  info.status != MN_STATUS_CONNECTED) {
        fprintf(stderr, "No connected peer became available.\n");
        goto cleanup;
    }

    if (!mn_send_reliable_message_ex(network, 0u, 1u, message,
                                     strlen(message), &delivery_id)) {
        fprintf(stderr, "Could not queue reliable chat message.\n");
        goto cleanup;
    }
    printf("Sent reliable chat message (delivery %u).\n",
           (unsigned)delivery_id);

    for (iteration = 0; iteration < 500u; ++iteration) {
        mn_update(network, 0.01f);
        print_events(network, delivery_id, &acknowledged, &failed);
        if (failed || (acknowledged && iteration >= 100u)) break;
        sleep_ms(10u);
    }
    result = acknowledged && !failed ? 0 : 1;

cleanup:
    mn_destroy(network);
    return result;
}
