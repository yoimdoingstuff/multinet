    instance->ready = 1;
    instance->info.status = MN_STATUS_OFFLINE;
    instance->info.role = MN_ROLE_NONE;
    instance->info.app_id = instance->config.app_id;
    instance->info.max_players = instance->config.max_players;
    instance->info.local_port = instance->local_port;
    instance->local_player.colour_rgba = 0xFFFFFFFFu;
    instance->local_player.rotation.w = 1.0f;
    return instance;
}

void mn_destroy(mn_instance *instance) {
    if (!instance) return;
    mn_leave(instance);
    mn_close_current_socket(instance);
#ifdef _WIN32
    WSACleanup();
#endif
    memset(instance, 0, sizeof(*instance));
    free(instance);
}

int mn_host(mn_instance *instance, const char *player_name,
            const char *room_name, uint8_t max_players) {
    mn_socket new_socket;
    uint32_t room_id;
    if (!instance || !instance->ready) return 0;
    if (instance->info.role == MN_ROLE_HOST) mn_leave(instance);
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    max_players = max_players < 2 ? 2 : (max_players > MN_MAX_PLAYERS ? MN_MAX_PLAYERS : max_players);
    mn_close_current_socket(instance);
    new_socket = mn_open_socket(instance, instance->config.port);
    if (new_socket == MN_BAD_SOCKET) {
        instance->socket = mn_open_socket(instance, 0);
        instance->info.local_port = instance->local_port;
        return 0;
    }
    instance->socket = new_socket;
    instance->info.local_port = instance->local_port;
    memset(instance->peers, 0, sizeof(instance->peers));
    memset(instance->players, 0, sizeof(instance->players));
    instance->room_count = 0;
    instance->session_token = 0;
    instance->next_player_id = 2;
    instance->info.role = MN_ROLE_HOST;
    instance->info.status = MN_STATUS_HOSTING;
    instance->info.local_player_id = 1;
    instance->info.room_id = 0;
    if (!mn_random_u32(&room_id)) {
        mn_set_error(instance, "Secure random number generation failed");
        mn_leave(instance);
        return 0;
    }
    instance->info.room_id = room_id;
    instance->info.max_players = max_players;
    instance->info.match_started = 0;
    mn_copy_text(instance->info.local_name, sizeof(instance->info.local_name), player_name);
    mn_copy_text(instance->info.room_name, sizeof(instance->info.room_name), room_name);
    memset(&instance->local_player, 0, sizeof(instance->local_player));
    instance->local_player.id = 1;
    instance->local_player.colour_rgba = 0xFFFFFFFFu;
    instance->local_player.rotation.w = 1.0f;
    mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name), player_name);
    instance->info.last_error[0] = '\0';
    instance->snapshot_tick = 0.0f;
    mn_refresh_host_players(instance);
    return 1;
}

int mn_discover(mn_instance *instance) {
    if (!instance || !instance->ready) return 0;
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before searching");
        return 0;
    }
    if (!instance->config.enable_broadcast) {
        mn_set_error(instance, "LAN broadcast discovery is disabled in this configuration");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    instance->room_count = 0;
    memset(instance->rooms, 0, sizeof(instance->rooms));
    instance->discovery_left = instance->config.discovery_seconds;
    instance->discovery_tick = 0.0f;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_SEARCHING;
    instance->info.last_error[0] = '\0';
    mn_send_discover(instance);
    return 1;
}
