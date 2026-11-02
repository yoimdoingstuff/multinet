    mn_emit_simple_event(instance, MN_EVENT_MATCH_STARTED, "Host started the match", 0);
    return 1;
}

void mn_leave(mn_instance *instance) {
    mn_role previous_role;
    if (!instance || !instance->ready) return;
    previous_role = instance->info.role;
    if (previous_role == MN_ROLE_CLIENT && instance->info.status == MN_STATUS_CONNECTED)
        mn_send_leave(instance);
    if (previous_role == MN_ROLE_HOST) {
        mn_close_current_socket(instance);
        instance->socket = mn_open_socket(instance, 0);
        instance->info.local_port = instance->local_port;
    }
    if (previous_role == MN_ROLE_CLIENT && instance->info.status == MN_STATUS_CONNECTED)
        mn_emit_simple_event(instance, MN_EVENT_DISCONNECTED, "Left the room", instance->info.local_player_id);
    memset(instance->peers, 0, sizeof(instance->peers));
    memset(instance->players, 0, sizeof(instance->players));
    memset(&instance->local_player, 0, sizeof(instance->local_player));
    instance->local_player.colour_rgba = 0xFFFFFFFFu;
    instance->local_player.rotation.w = 1.0f;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_OFFLINE;
    instance->info.local_player_id = 0;
    instance->info.room_id = 0;
    instance->info.player_count = 0;
    instance->info.max_players = instance->config.max_players;
    instance->info.match_started = 0;
    instance->info.room_name[0] = '\0';
    instance->info.local_name[0] = '\0';
    instance->info.last_error[0] = '\0';
    instance->session_token = 0;
    instance->room_count = 0;
    instance->discovery_left = 0.0f;
    instance->connect_elapsed = 0.0f;
    instance->server_silence = 0.0f;
}

int mn_set_local_player(mn_instance *instance, const mn_player *player) {
    if (!instance || !player || !mn_player_is_valid(player)) return 0;
    instance->local_player = *player;
    instance->local_player.id = instance->info.local_player_id;
    mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name),
                 instance->info.local_name);
    if (instance->info.role == MN_ROLE_HOST) mn_refresh_host_players(instance);
    return 1;
}

void mn_get_info(const mn_instance *instance, mn_info *out_info) {
    if (instance && out_info) *out_info = instance->info;
}

size_t mn_get_players(const mn_instance *instance, mn_player *players, size_t capacity) {
    size_t count;
    if (!instance) return 0;
    count = instance->info.player_count < capacity ? instance->info.player_count : capacity;
    if (players && count) memcpy(players, instance->players, count * sizeof(mn_player));
    return instance->info.player_count;
}

int mn_get_player(const mn_instance *instance, uint32_t player_id, mn_player *out_player) {
    size_t i;
    if (!instance || !out_player || player_id == 0) return 0;
    for (i = 0; i < instance->info.player_count && i < MN_MAX_PLAYERS; ++i) {
        if (instance->players[i].id == player_id) {
            *out_player = instance->players[i];
            return 1;
        }
    }
    return 0;
}

int mn_poll_event(mn_instance *instance, mn_event *out_event) {
    if (!instance || !out_event || instance->event_count == 0) return 0;
    *out_event = instance->events[instance->event_head];
    memset(&instance->events[instance->event_head], 0, sizeof(mn_event));
    instance->event_head = (instance->event_head + 1u) % MN_EVENT_QUEUE_CAPACITY;
    instance->event_count--;
    return 1;
}

int mn_send_message(mn_instance *instance, uint32_t target_player_id,
                    uint16_t channel, const void *data, size_t size) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[20u + MN_MAX_MESSAGE_BYTES];
    size_t packet_size;
    if (!instance || !instance->ready || size > MN_MAX_MESSAGE_BYTES ||
        (size > 0 && !data)) return 0;
    if (instance->info.role == MN_ROLE_HOST && instance->info.status == MN_STATUS_HOSTING) {
        if (target_player_id != 0 && target_player_id != instance->info.local_player_id &&
            !mn_find_peer_id(instance, target_player_id)) return 0;
        mn_emit_message(instance, instance->info.local_player_id, channel,
                        (const uint8_t *)data, size);
        mn_forward_message(instance, instance->info.local_player_id, target_player_id,
                           channel, (const uint8_t *)data, size);
        return 1;
    }
    if (instance->info.role != MN_ROLE_CLIENT ||
        instance->info.status != MN_STATUS_CONNECTED || target_player_id != 0)
        return 0;
    mn_write_u64(payload, instance->session_token);
    mn_write_u32(payload + 8, instance->info.local_player_id);
    mn_write_u32(payload + 12, target_player_id);
    mn_write_u16(payload + 16, channel);
    mn_write_u16(payload + 18, (uint16_t)size);
    if (size) memcpy(payload + 20, data, size);
    packet_size = mn_make_packet(packet, MN_PKT_MESSAGE, instance->config.app_id,
                                 instance->info.room_id, payload, 20u + size);
    mn_emit_message(instance, instance->info.local_player_id, channel,
                    (const uint8_t *)data, size);
