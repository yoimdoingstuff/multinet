
static void mn_handle_client_message(mn_instance *instance, const uint8_t *payload,
                                     size_t payload_size) {
    uint32_t sender_id, target_id;
    uint16_t channel, data_size;
    if (payload_size < 12u) return;
    sender_id = mn_read_u32(payload);
    target_id = mn_read_u32(payload + 4);
    channel = mn_read_u16(payload + 8);
    data_size = mn_read_u16(payload + 10);
    if ((size_t)data_size > MN_MAX_MESSAGE_BYTES || payload_size != 12u + data_size)
        return;
    if (target_id != 0 && target_id != instance->info.local_player_id) return;
    mn_emit_message(instance, sender_id, channel, payload + 12, data_size);
}

static void mn_handle_packet(mn_instance *instance, const uint8_t *packet,
                             size_t packet_size, const struct sockaddr_in *from) {
    uint8_t type;
    uint32_t app_id, room_id;
    const uint8_t *payload;
    size_t payload_size;
    if (packet_size < MN_HEADER_SIZE) return;
    if (mn_read_u32(packet) != MN_MAGIC || packet[4] != MN_PROTOCOL_VERSION ||
        mn_read_u16(packet + 6) != packet_size) return;
    type = packet[5];
    app_id = mn_read_u32(packet + 8);
    room_id = mn_read_u32(packet + 12);
    if (app_id != instance->config.app_id) return;
    payload = packet + MN_HEADER_SIZE;
    payload_size = packet_size - MN_HEADER_SIZE;

    if (type == MN_PKT_DISCOVER && payload_size == 0 &&
        instance->info.role == MN_ROLE_HOST &&
        instance->info.status == MN_STATUS_HOSTING) {
        mn_send_offer(instance, from);
        return;
    }
    if (type == MN_PKT_OFFER && payload_size == MN_OFFER_SIZE &&
        instance->info.status == MN_STATUS_SEARCHING) {
        mn_add_room(instance, from, room_id, payload);
        return;
    }

    if (instance->info.role == MN_ROLE_HOST) {
        if (type == MN_PKT_JOIN && room_id == 0 && payload_size == MN_PLAYER_NAME_LENGTH) {
            mn_peer *peer = mn_find_peer_address(instance, from);
            uint8_t name[MN_PLAYER_NAME_LENGTH];
            size_t i;
            memcpy(name, payload, sizeof(name));
            name[sizeof(name) - 1] = '\0';
            if (peer) {
                peer->silence = 0.0f;
                mn_send_welcome(instance, peer);
                mn_send_snapshot_to(instance, from);
                return;
            }
            if (mn_active_player_count(instance) >= instance->info.max_players) return;
            for (i = 0; i < MN_MAX_PLAYERS && instance->peers[i].active; ++i) { }
            if (i == MN_MAX_PLAYERS) return;
            peer = &instance->peers[i];
            memset(peer, 0, sizeof(*peer));
            peer->active = 1;
            peer->id = instance->next_player_id++;
            peer->address = *from;
            peer->player.id = peer->id;
            peer->player.colour_rgba = 0xFFFFFFFFu;
            peer->player.rotation.w = 1.0f;
            memcpy(peer->player.name, name, sizeof(name));
            peer->player.name[MN_PLAYER_NAME_LENGTH - 1] = '\0';
            if (!mn_random_u64(&peer->token)) {
                memset(peer, 0, sizeof(*peer));
                mn_set_error(instance, "Secure random number generation failed");
                return;
            }
            instance->info.player_count = (uint8_t)mn_active_player_count(instance);
            {
                mn_event event;
                memset(&event, 0, sizeof(event));
                event.type = MN_EVENT_PLAYER_JOINED;
                event.player_id = peer->id;
                event.player = peer->player;
                mn_push_event(instance, &event);
            }
