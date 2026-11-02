
void mn_stop_discovery(mn_instance *instance) {
    if (!instance || instance->info.status != MN_STATUS_SEARCHING) return;
    instance->info.status = MN_STATUS_OFFLINE;
    instance->info.role = MN_ROLE_NONE;
    instance->discovery_left = 0.0f;
}

size_t mn_get_rooms(const mn_instance *instance, mn_room *rooms, size_t capacity) {
    size_t count;
    if (!instance) return 0;
    count = instance->room_count < capacity ? instance->room_count : capacity;
    if (rooms && count) memcpy(rooms, instance->rooms, count * sizeof(mn_room));
    return instance->room_count;
}

int mn_join(mn_instance *instance, const char *host, uint16_t port,
            const char *player_name) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct sockaddr_in server;
    int resolve_result;
    if (!instance || !instance->ready) return 0;
    if (!host || !host[0]) {
        mn_set_error(instance, "Host address is empty");
        return 0;
    }
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before joining another room");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    resolve_result = getaddrinfo(host, NULL, &hints, &result);
    if (resolve_result != 0 || !result) {
        mn_set_error(instance, "Could not resolve the host as an IPv4 address");
        if (result) freeaddrinfo(result);
        return 0;
    }
    memset(&server, 0, sizeof(server));
    memcpy(&server, result->ai_addr, sizeof(server));
    server.sin_port = htons(port ? port : instance->config.port);
    freeaddrinfo(result);
    instance->server = server;
    mn_copy_text(instance->info.local_name, sizeof(instance->info.local_name), player_name);
    mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name), player_name);
    instance->local_player.id = 0;
    if (instance->local_player.colour_rgba == 0) instance->local_player.colour_rgba = 0xFFFFFFFFu;
    if (instance->local_player.rotation.w == 0.0f) instance->local_player.rotation.w = 1.0f;
    instance->info.role = MN_ROLE_CLIENT;
    instance->info.status = MN_STATUS_CONNECTING;
    instance->info.local_player_id = 0;
    instance->info.room_id = 0;
    instance->info.player_count = 0;
    instance->info.match_started = 0;
    instance->info.max_players = instance->config.max_players;
    instance->info.room_name[0] = '\0';
    instance->session_token = 0;
    instance->connect_elapsed = 0.0f;
    instance->server_silence = 0.0f;
    instance->heartbeat_tick = 0.0f;
    instance->info.last_error[0] = '\0';
    memset(instance->players, 0, sizeof(instance->players));
    mn_send_join(instance);
    return 1;
}

int mn_join_room(mn_instance *instance, const mn_room *room,
                 const char *player_name) {
    if (!room) return 0;
    return mn_join(instance, room->address, room->port, player_name);
}

int mn_start_match(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    size_t size, i;
    if (!instance || !instance->ready || instance->info.role != MN_ROLE_HOST ||
        instance->info.status != MN_STATUS_HOSTING) {
        if (instance) mn_set_error(instance, "Only the room host can start the match");
        return 0;
    }
    if (instance->info.match_started) return 1;
    instance->info.match_started = 1;
    size = mn_make_packet(packet, MN_PKT_START, instance->config.app_id,
                          instance->info.room_id, NULL, 0);
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active)
            mn_send_to(instance, &instance->peers[i].address, packet, size);
    }
