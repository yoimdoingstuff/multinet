
    mn_send_to(instance, &instance->server, packet, packet_size);
    return packet_size != 0;
}

void mn_update(mn_instance *instance, float dt_seconds) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    struct sockaddr_in from;
    mn_socklen from_length;
    int received;
    size_t i;
    float dt = dt_seconds;
    if (!instance || !instance->ready || instance->socket == MN_BAD_SOCKET) return;
    if (!mn_float_is_valid(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;

    for (;;) {
        from_length = (mn_socklen)sizeof(from);
        received = (int)recvfrom(instance->socket, (char *)packet,
                                 (int)sizeof(packet), 0,
                                 (struct sockaddr *)&from, &from_length);
        if (received <= 0) break;
        mn_handle_packet(instance, packet, (size_t)received, &from);
    }

    if (instance->info.status == MN_STATUS_SEARCHING) {
        instance->discovery_left -= dt;
        instance->discovery_tick -= dt;
        if (instance->discovery_left > 0.0f && instance->discovery_tick <= 0.0f) {
            mn_send_discover(instance);
            instance->discovery_tick = 0.5f;
        }
        if (instance->discovery_left <= 0.0f) {
            instance->info.status = MN_STATUS_OFFLINE;
            instance->info.role = MN_ROLE_NONE;
        }
    }

    if (instance->info.role == MN_ROLE_CLIENT &&
        instance->info.status == MN_STATUS_CONNECTING) {
        instance->connect_elapsed += dt;
        if (instance->connect_elapsed > MN_CONNECT_TIMEOUT) {
            instance->info.role = MN_ROLE_NONE;
            instance->info.status = MN_STATUS_OFFLINE;
            mn_set_error(instance, "Connection timed out");
        } else {
            instance->heartbeat_tick -= dt;
            if (instance->heartbeat_tick <= 0.0f) {
                mn_send_join(instance);
                instance->heartbeat_tick = 0.5f;
            }
        }
    } else if (instance->info.role == MN_ROLE_CLIENT &&
               instance->info.status == MN_STATUS_CONNECTED) {
        instance->server_silence += dt;
        if (instance->server_silence > instance->config.timeout_seconds) {
            uint32_t player_id = instance->info.local_player_id;
            instance->info.role = MN_ROLE_NONE;
            instance->info.status = MN_STATUS_OFFLINE;
            instance->info.player_count = 0;
            instance->info.match_started = 0;
            memset(instance->players, 0, sizeof(instance->players));
            mn_emit_simple_event(instance, MN_EVENT_DISCONNECTED,
                                 "Host disconnected", player_id);
            mn_set_error(instance, "Host disconnected");
        } else {
            instance->heartbeat_tick -= dt;
            if (instance->heartbeat_tick <= 0.0f) {
                mn_send_state(instance);
                instance->heartbeat_tick = instance->config.heartbeat_seconds;
            }
