/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet.h"

#include <math.h>
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

int main(int argc, char **argv) {
    mn_config config;
    mn_instance *network = NULL;
    mn_info info;
    mn_player local;
    mn_player players[MN_MAX_PLAYERS];
    const char *name;
    int is_host;
    float elapsed = 0.0f;
    float report_elapsed = 0.0f;
    float direction = 1.0f;
    unsigned iteration;
    size_t player_count;
    size_t i;
    int result = 1;

    if (argc < 3 || argc > 4) {
        fprintf(stderr,
            "Usage:\n"
            "  %s host <name>\n"
            "  %s join <host-ip> <name>\n",
            argv[0], argv[0]);
        return 2;
    }
    is_host = strcmp(argv[1], "host") == 0;
    if (!is_host && strcmp(argv[1], "join") != 0) {
        fprintf(stderr, "Mode must be 'host' or 'join'.\n");
        return 2;
    }
    if (is_host) {
        if (argc != 3) return 2;
        name = argv[2];
    } else {
        if (argc != 4) return 2;
        name = argv[3];
    }

    mn_config_init(&config);
    network = mn_create(&config);
    if (!network) {
        fprintf(stderr, "Could not create MultiNet instance.\n");
        return 1;
    }
    if (is_host) {
        if (!mn_host(network, name, "2D Object Demo", MN_MAX_PLAYERS)) {
            mn_get_info(network, &info);
            fprintf(stderr, "Could not host: %s\n", info.last_error);
            goto cleanup;
        }
        printf("Hosting 2D object demo on UDP %u.\n",
               (unsigned)config.port);
    } else {
        if (!mn_join(network, argv[2], 0u, name)) {
            mn_get_info(network, &info);
            fprintf(stderr, "Could not start join: %s\n", info.last_error);
            goto cleanup;
        }
        printf("Joining %s...\n", argv[2]);
    }

    for (iteration = 0; iteration < 3000u; ++iteration) {
        mn_update(network, 0.01f);
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

    memset(&local, 0, sizeof(local));
    local.rotation.w = 1.0f;
    local.colour_rgba = 0x35C7B8FFu;
    printf("Replicating a moving 2D point for 30 seconds. X/Y are world coordinates.\n");

    for (iteration = 0; iteration < 3000u; ++iteration) {
        local.position.x += direction * 0.08f;
        if (local.position.x > 8.0f) {
            local.position.x = 8.0f;
            direction = -1.0f;
        } else if (local.position.x < -8.0f) {
            local.position.x = -8.0f;
            direction = 1.0f;
        }
        local.position.y = sinf(elapsed * 0.7f) * 4.0f;
        (void)mn_set_local_player(network, &local);
        mn_update(network, 0.01f);

        report_elapsed += 0.01f;
        if (report_elapsed >= 1.0f) {
            report_elapsed = 0.0f;
            player_count = mn_get_players(network, players, MN_MAX_PLAYERS);
            printf("\nRoom objects (%u):\n", (unsigned)player_count);
            for (i = 0; i < player_count && i < MN_MAX_PLAYERS; ++i) {
                printf("  #%u %-23s x=%7.2f y=%7.2f\n",
                       (unsigned)players[i].id, players[i].name,
                       (double)players[i].position.x,
                       (double)players[i].position.y);
            }
        }
        elapsed += 0.01f;
        sleep_ms(10u);
    }
    result = 0;

cleanup:
    mn_destroy(network);
    return result;
}
