
static int mn_decode_player(mn_player *player, const uint8_t *in) {
    size_t offset = 0;
    size_t i;
    memset(player, 0, sizeof(*player));
    player->id = mn_read_u32(in + offset); offset += 4;
    player->score = (int32_t)mn_read_u32(in + offset); offset += 4;
    player->flags = mn_read_u32(in + offset); offset += 4;
    player->colour_rgba = mn_read_u32(in + offset); offset += 4;
    for (i = 0; i < 4; ++i) { player->appearance[i] = mn_read_u16(in + offset); offset += 2; }
    memcpy(player->name, in + offset, MN_PLAYER_NAME_LENGTH);
    player->name[MN_PLAYER_NAME_LENGTH - 1] = '\0';
    offset += MN_PLAYER_NAME_LENGTH;
    player->position.x = mn_read_float(in + offset); offset += 4;
    player->position.y = mn_read_float(in + offset); offset += 4;
    player->position.z = mn_read_float(in + offset); offset += 4;
    player->rotation.x = mn_read_float(in + offset); offset += 4;
    player->rotation.y = mn_read_float(in + offset); offset += 4;
    player->rotation.z = mn_read_float(in + offset); offset += 4;
    player->rotation.w = mn_read_float(in + offset); offset += 4;
    player->velocity.x = mn_read_float(in + offset); offset += 4;
    player->velocity.y = mn_read_float(in + offset); offset += 4;
    player->velocity.z = mn_read_float(in + offset); offset += 4;
    return mn_player_is_valid(player);
}

static size_t mn_make_packet(uint8_t *out, uint8_t type, uint32_t app_id,
                             uint32_t room_id, const void *payload,
                             size_t payload_size) {
    size_t total = MN_HEADER_SIZE + payload_size;
    if (!out || total > MN_MAX_DATAGRAM_SIZE || payload_size > 65535u) return 0;
    mn_write_u32(out, MN_MAGIC);
    out[4] = MN_PROTOCOL_VERSION;
    out[5] = type;
    mn_write_u16(out + 6, (uint16_t)total);
    mn_write_u32(out + 8, app_id);
    mn_write_u32(out + 12, room_id);
    if (payload_size && payload) memcpy(out + MN_HEADER_SIZE, payload, payload_size);
    return total;
}

static void mn_send_to(mn_instance *instance, const struct sockaddr_in *to,
                       const uint8_t *data, size_t size) {
    if (instance && instance->socket != MN_BAD_SOCKET && to && data && size) {
#ifdef _WIN32
        (void)sendto(instance->socket, (const char *)data, (int)size, 0,
                     (const struct sockaddr *)to, (int)sizeof(*to));
#else
        (void)sendto(instance->socket, (const char *)data, size, 0,
                     (const struct sockaddr *)to, (socklen_t)sizeof(*to));
#endif
    }
}

static size_t mn_active_player_count(const mn_instance *instance) {
    size_t count = instance->info.role == MN_ROLE_HOST ? 1u : 0u;
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i)
        if (instance->peers[i].active) ++count;
    return count;
}

static void mn_refresh_host_players(mn_instance *instance) {
    size_t i, count = 0;
    memset(instance->players, 0, sizeof(instance->players));
    if (instance->info.role == MN_ROLE_HOST) instance->players[count++] = instance->local_player;
    for (i = 0; i < MN_MAX_PLAYERS && count < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active) instance->players[count++] = instance->peers[i].player;
    }
    instance->info.player_count = (uint8_t)count;
}

static void mn_send_offer(mn_instance *instance, const struct sockaddr_in *to) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[MN_OFFER_SIZE];
    size_t size;
    payload[0] = instance->info.player_count;
    payload[1] = instance->info.max_players;
    memset(payload + 2, 0, MN_ROOM_NAME_LENGTH + MN_PLAYER_NAME_LENGTH);
    mn_copy_text((char *)(payload + 2), MN_ROOM_NAME_LENGTH, instance->info.room_name);
    mn_copy_text((char *)(payload + 2 + MN_ROOM_NAME_LENGTH),
                 MN_PLAYER_NAME_LENGTH, instance->info.local_name);
    size = mn_make_packet(packet, MN_PKT_OFFER, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, to, packet, size);
}
