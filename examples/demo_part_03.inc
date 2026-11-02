
            mn_get_info(network, &info);
            count = mn_get_players(network, players, MN_MAX_PLAYERS);
            printf("\n[%s] %s | players %u/%u | local ball (%.1f, %.1f, %.1f)\n",
                   info.role == MN_ROLE_HOST ? "HOST" : "CLIENT",
                   info.status == MN_STATUS_HOSTING ? "hosting" :
                   info.status == MN_STATUS_CONNECTED ? "connected" :
                   info.status == MN_STATUS_CONNECTING ? "connecting" : "offline",
                   (unsigned)info.player_count, (unsigned)info.max_players,
                   local.position.x, local.position.y, local.position.z);
            for (i = 0; i < count && i < MN_MAX_PLAYERS; ++i) {
                printf("  #%u %-22s pos=(%6.1f, %6.1f, %6.1f) score=%d%s\n",
                       (unsigned)players[i].id, players[i].name,
                       players[i].position.x, players[i].position.y,
                       players[i].position.z, players[i].score,
                       (players[i].flags & 1u) ? "  [moving]" : "");
            }
            if (info.last_error[0]) printf("  last error: %s\n", info.last_error);
            fflush(stdout);
            last_print = now;
        }
        sleep_ms(16);
        if (info.status == MN_STATUS_OFFLINE && !hosting) {
            mn_get_info(network, &info);
            if (info.status == MN_STATUS_OFFLINE && info.last_error[0]) running = 0;
        }
    }
    terminal_end();
    mn_leave(network);
    mn_destroy(network);
    printf("\nMultiNet demo closed.\n");
    return 0;
}

int main(int argc, char **argv) {
    mn_config config;
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return argc < 2 ? 0 : 0;
    }
    mn_config_init(&config);
    if (strcmp(argv[1], "scan") == 0) {
        config.port = argc >= 3 ? parse_port(argv[2], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_scan(&config);
    }
    if (strcmp(argv[1], "host") == 0) {
        const char *name = argc >= 3 ? argv[2] : "Host";
        const char *room = argc >= 4 ? argv[3] : "MultiNet Test Room";
        config.port = argc >= 5 ? parse_port(argv[4], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_session(&config, 1, NULL, name, room, MN_MAX_PLAYERS);
    }
    if (strcmp(argv[1], "join") == 0 && argc >= 4) {
        const char *address = argv[2];
        const char *name = argv[3];
        config.port = argc >= 5 ? parse_port(argv[4], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_session(&config, 0, address, name, NULL, MN_MAX_PLAYERS);
    }
    print_usage(argv[0]);
    return 2;
}