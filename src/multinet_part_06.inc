
static void mn_send_discover(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    struct sockaddr_in destination;
    size_t size;
    if (!instance->config.enable_broadcast) return;
    size = mn_make_packet(packet, MN_PKT_DISCOVER, instance->config.app_id,
                          0, NULL, 0);
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(instance->config.port);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    mn_send_to(instance, &destination, packet, size);
}

static void mn_send_join(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[MN_PLAYER_NAME_LENGTH];
    size_t size;
    memset(payload, 0, sizeof(payload));
    mn_copy_text((char *)payload, sizeof(payload), instance->info.local_name);
    size = mn_make_packet(packet, MN_PKT_JOIN, instance->config.app_id,
                          0, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, size);
}

static void mn_send_state(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[8u + MN_WIRE_PLAYER_SIZE];
    size_t size;
    mn_write_u64(payload, instance->session_token);
    mn_encode_player(payload + 8, &instance->local_player);
    size = mn_make_packet(packet, MN_PKT_STATE, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, size);
}

static void mn_send_leave(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[8];
    size_t size;
    mn_write_u64(payload, instance->session_token);
    size = mn_make_packet(packet, MN_PKT_LEAVE, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, size);
}

static void mn_emit_message(mn_instance *instance, uint32_t sender_id,
                            uint16_t channel, const uint8_t *data, size_t size) {
    mn_event event;
    if (size > MN_MAX_MESSAGE_BYTES) return;
    memset(&event, 0, sizeof(event));
    event.type = MN_EVENT_MESSAGE;
    event.player_id = sender_id;
    event.channel = channel;
    event.data_size = (uint16_t)size;
    if (size) memcpy(event.data, data, size);
    mn_push_event(instance, &event);
}

static void mn_forward_message(mn_instance *instance, uint32_t sender_id,
                               uint32_t target_id, uint16_t channel,
                               const uint8_t *data, size_t size) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[12u + MN_MAX_MESSAGE_BYTES];
    size_t i, packet_size;
    mn_write_u32(payload, sender_id);
    mn_write_u32(payload + 4, target_id);
    mn_write_u16(payload + 8, channel);
    mn_write_u16(payload + 10, (uint16_t)size);
    if (size) memcpy(payload + 12, data, size);
    packet_size = mn_make_packet(packet, MN_PKT_MESSAGE,
                                 instance->config.app_id, instance->info.room_id,
                                 payload, 12u + size);
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        mn_peer *peer = &instance->peers[i];
        if (!peer->active || peer->id == sender_id) continue;
        if (target_id != 0 && target_id != peer->id) continue;
        mn_send_to(instance, &peer->address, packet, packet_size);
    }
}

static void mn_handle_host_message(mn_instance *instance, const uint8_t *payload,
                                   size_t payload_size,
                                   const struct sockaddr_in *from) {
    uint64_t token;
    uint32_t sender_id, target_id;
    uint16_t channel, data_size;
    mn_peer *peer;
    if (payload_size < 20u) return;
    token = mn_read_u64(payload);
    sender_id = mn_read_u32(payload + 8);
    target_id = mn_read_u32(payload + 12);
    channel = mn_read_u16(payload + 16);
    data_size = mn_read_u16(payload + 18);
    if ((size_t)data_size > MN_MAX_MESSAGE_BYTES || payload_size != 20u + data_size)
        return;
    peer = mn_find_peer_token(instance, token);
    if (!peer || !mn_same_address(&peer->address, from) || sender_id != peer->id) return;
    peer->silence = 0.0f;
    if (target_id != 0 && target_id != instance->info.local_player_id &&
        !mn_find_peer_id(instance, target_id)) return;
    mn_emit_message(instance, peer->id, channel, payload + 20, data_size);
    mn_forward_message(instance, peer->id, target_id, channel,
                        payload + 20, data_size);
}
