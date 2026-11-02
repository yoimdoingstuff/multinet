
static void mn_send_welcome(mn_instance *instance, const mn_peer *peer) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[MN_WELCOME_SIZE];
    size_t size;
    mn_write_u32(payload, peer->id);
    mn_write_u64(payload + 4, peer->token);
    payload[12] = instance->info.max_players;
    memset(payload + 13, 0, MN_ROOM_NAME_LENGTH);
    mn_copy_text((char *)(payload + 13), MN_ROOM_NAME_LENGTH, instance->info.room_name);
    size = mn_make_packet(packet, MN_PKT_WELCOME, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &peer->address, packet, size);
}

static void mn_send_snapshot_to(mn_instance *instance,
                                const struct sockaddr_in *to) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[2u + MN_MAX_PLAYERS * MN_WIRE_PLAYER_SIZE];
    size_t i, count = 0, size;
    payload[1] = instance->info.match_started ? 1u : 0u;
    mn_encode_player(payload + 2u + count * MN_WIRE_PLAYER_SIZE,
                     &instance->local_player);
    ++count;
    for (i = 0; i < MN_MAX_PLAYERS && count < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active) {
            mn_encode_player(payload + 2u + count * MN_WIRE_PLAYER_SIZE,
                             &instance->peers[i].player);
            ++count;
        }
    }
    payload[0] = (uint8_t)count;
    size = mn_make_packet(packet, MN_PKT_SNAPSHOT, instance->config.app_id,
                          instance->info.room_id, payload,
                          2u + count * MN_WIRE_PLAYER_SIZE);
    mn_send_to(instance, to, packet, size);
}

static void mn_broadcast_snapshot(mn_instance *instance) {
    size_t i;
    mn_refresh_host_players(instance);
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active)
            mn_send_snapshot_to(instance, &instance->peers[i].address);
    }
}

static void mn_remove_peer(mn_instance *instance, mn_peer *peer,
                           const char *reason) {
    uint32_t player_id;
    if (!peer || !peer->active) return;
    player_id = peer->id;
    mn_emit_simple_event(instance, MN_EVENT_PLAYER_LEFT, reason, player_id);
    memset(peer, 0, sizeof(*peer));
    mn_refresh_host_players(instance);
}

static void mn_add_room(mn_instance *instance, const struct sockaddr_in *from,
                        uint32_t room_id, const uint8_t *payload) {
    char ip[48];
    size_t i;
    mn_room room;
    if (!inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip))) return;
    if (payload[1] < 2u || payload[1] > MN_MAX_PLAYERS || payload[0] == 0u ||
        payload[0] > payload[1]) return;
    memset(&room, 0, sizeof(room));
    memcpy(room.name, payload + 2, MN_ROOM_NAME_LENGTH);
    room.name[MN_ROOM_NAME_LENGTH - 1] = '\0';
    memcpy(room.host_name, payload + 2 + MN_ROOM_NAME_LENGTH, MN_PLAYER_NAME_LENGTH);
    room.host_name[MN_PLAYER_NAME_LENGTH - 1] = '\0';
    mn_copy_text(room.address, sizeof(room.address), ip);
    room.port = ntohs(from->sin_port);
    room.player_count = payload[0];
    room.max_players = payload[1];
    room.room_id = room_id;
    room.app_id = instance->config.app_id;
    for (i = 0; i < instance->room_count; ++i) {
        if (instance->rooms[i].room_id == room_id &&
            strcmp(instance->rooms[i].address, ip) == 0) {
            instance->rooms[i] = room;
            return;
        }
    }
    if (instance->room_count >= MN_MAX_ROOMS) return;
    instance->rooms[instance->room_count++] = room;
    {
        mn_event event;
        memset(&event, 0, sizeof(event));
        event.type = MN_EVENT_ROOM_FOUND;
        mn_copy_text(event.text, sizeof(event.text), room.name);
        mn_push_event(instance, &event);
    }
}
