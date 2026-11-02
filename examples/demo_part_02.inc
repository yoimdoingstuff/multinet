                printf("\n[match started] %s\n", event.text);
                break;
            case MN_EVENT_MESSAGE:
                printf("\n[message] from player #%u on channel %u: ",
                       (unsigned)event.player_id, (unsigned)event.channel);
                if (event.data_size) fwrite(event.data, 1, event.data_size, stdout);
                putchar('\n');
                break;
            case MN_EVENT_ERROR:
                printf("\n[network error] %s\n", event.text);
                break;
            default:
                break;
        }
    }
}

static int run_scan(const mn_config *config) {
    mn_instance *network = mn_create(config);
    double start, previous;
    if (!network) {
        fprintf(stderr, "Could not initialise MultiNet. Check socket permissions/firewall.\n");
        return 1;
    }
    if (!mn_discover(network)) {
        mn_info info;
        mn_get_info(network, &info);
        fprintf(stderr, "Discovery failed: %s\n", info.last_error);
        mn_destroy(network);
        return 1;
    }
    printf("Searching for MultiNet rooms on UDP port %u...\n", (unsigned)config->port);
    start = monotonic_seconds();
    previous = start;
    while (monotonic_seconds() - start < config->discovery_seconds + 0.2) {
        double now = monotonic_seconds();
        mn_update(network, (float)(now - previous));
        previous = now;
        drain_events(network);
        sleep_ms(20);
    }
    print_rooms(network);
    mn_destroy(network);
    return 0;
}

static int run_session(const mn_config *config, int hosting,
                       const char *address, const char *player_name,
                       const char *room_name, uint8_t max_players) {
    mn_instance *network = mn_create(config);
    mn_player local;
    mn_info info;
    double previous, last_print;
    int running = 1;
    if (!network) {
        fprintf(stderr, "Could not initialise MultiNet. Check socket permissions/firewall.\n");
        return 1;
    }
    if (hosting) {
        if (!mn_host(network, player_name, room_name, max_players)) {
            mn_get_info(network, &info);
            fprintf(stderr, "Host failed: %s\n", info.last_error);
            mn_destroy(network);
            return 1;
        }
        printf("Hosting '%s' as %s on UDP %u.\n", room_name, player_name,
               (unsigned)config->port);
    } else {
        if (!mn_join(network, address, config->port, player_name)) {
            mn_get_info(network, &info);
            fprintf(stderr, "Join failed: %s\n", info.last_error);
            mn_destroy(network);
            return 1;
        }
        printf("Joining %s:%u as %s...\n", address, (unsigned)config->port,
               player_name);
    }

    memset(&local, 0, sizeof(local));
    local.colour_rgba = hosting ? 0x35C7B8FFu : 0xFFB84DFFu;
    local.rotation.w = 1.0f;
    printf("Controls: WASD move, M start match (host), B send message, Q quit.\n");
    printf("Press keys directly in this window.\n\n");
    terminal_begin();
    previous = monotonic_seconds();
    last_print = previous;
    while (running) {
        double now = monotonic_seconds();
        float dt = (float)(now - previous);
        int key;
        mn_player players[MN_MAX_PLAYERS];
        size_t count, i;
        previous = now;
        while ((key = read_key()) >= 0) {
            if (key == 'q' || key == 'Q') running = 0;
            else if (key == 'w' || key == 'W') local.position.y += 0.5f;
            else if (key == 's' || key == 'S') local.position.y -= 0.5f;
            else if (key == 'a' || key == 'A') local.position.x -= 0.5f;
            else if (key == 'd' || key == 'D') local.position.x += 0.5f;
            else if ((key == 'm' || key == 'M') && hosting) {
                if (!mn_start_match(network)) printf("\nCould not start match.\n");
            } else if (key == 'b' || key == 'B') {
                static const char message[] = "Hello from the MultiNet demo!";
                (void)mn_send_message(network, 0, 1, message, sizeof(message) - 1);
            }
        }
        (void)mn_set_local_player(network, &local);
        mn_update(network, dt);
        mn_get_info(network, &info);
        drain_events(network);
        if (now - last_print >= 1.0) {