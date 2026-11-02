
            for (i = 0; i < count; ++i) {
                size_t old_index;
                int found = 0;
                if (incoming[i].id == instance->info.local_player_id) continue;
                for (old_index = 0; old_index < instance->info.player_count; ++old_index) {
                    if (instance->players[old_index].id == incoming[i].id) { found = 1; break; }
                }
                if (!found) {
                    mn_event event;
                    memset(&event, 0, sizeof(event));
                    event.type = MN_EVENT_PLAYER_JOINED;
                    event.player_id = incoming[i].id;
                    event.player = incoming[i];
                    mn_push_event(instance, &event);
                }
            }
            for (i = 0; i < instance->info.player_count; ++i) {
                size_t new_index;
                uint32_t old_id = instance->players[i].id;
                int found = 0;
                if (old_id == 0 || old_id == instance->info.local_player_id) continue;
                for (new_index = 0; new_index < count; ++new_index) {
                    if (incoming[new_index].id == old_id) { found = 1; break; }
                }
                if (!found) mn_emit_simple_event(instance, MN_EVENT_PLAYER_LEFT,
                                                  "Player left the room", old_id);
            }
            memset(instance->players, 0, sizeof(instance->players));
            memcpy(instance->players, incoming, count * sizeof(mn_player));
            instance->info.player_count = (uint8_t)count;
            instance->info.match_started = payload[1];
            if (!was_started && instance->info.match_started)
                mn_emit_simple_event(instance, MN_EVENT_MATCH_STARTED,
                                     "Host started the match", 0);
            return;
        }
        if (type == MN_PKT_MESSAGE) {
            mn_handle_client_message(instance, payload, payload_size);
            return;
        }
    }
}

void mn_config_init(mn_config *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->port = MN_DEFAULT_PORT;
    config->app_id = MN_DEFAULT_APP_ID;
    config->max_players = MN_MAX_PLAYERS;
    config->enable_broadcast = 1;
    config->discovery_seconds = 3.0f;
    config->heartbeat_seconds = 0.10f;
    config->timeout_seconds = 8.0f;
}

mn_instance *mn_create(const mn_config *config) {
    mn_instance *instance;
    mn_config defaults;
    mn_config_init(&defaults);
    instance = (mn_instance *)calloc(1, sizeof(*instance));
    if (!instance) return NULL;
    instance->config = config ? *config : defaults;
    if (instance->config.port == 0) instance->config.port = defaults.port;
    if (instance->config.app_id == 0) instance->config.app_id = defaults.app_id;
    if (instance->config.max_players < 2 || instance->config.max_players > MN_MAX_PLAYERS)
        instance->config.max_players = MN_MAX_PLAYERS;
    if (instance->config.discovery_seconds <= 0.0f) instance->config.discovery_seconds = defaults.discovery_seconds;
    if (instance->config.heartbeat_seconds <= 0.0f) instance->config.heartbeat_seconds = defaults.heartbeat_seconds;
    if (instance->config.timeout_seconds <= 0.0f) instance->config.timeout_seconds = defaults.timeout_seconds;
    instance->socket = MN_BAD_SOCKET;
#ifdef _WIN32
    {
        WSADATA winsock_data;
        if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
            free(instance);
            return NULL;
        }
    }
#endif
    instance->socket = mn_open_socket(instance, 0);
    if (instance->socket == MN_BAD_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        free(instance);
        return NULL;
    }
