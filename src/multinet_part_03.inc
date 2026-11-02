    if (!mn_set_nonblocking(socket_handle)) {
        mn_set_error(instance, "Could not make the UDP socket non-blocking");
        mn_close_socket(socket_handle);
        return MN_BAD_SOCKET;
    }
    if (instance->config.enable_broadcast) {
        int broadcast_enabled = 1;
        (void)setsockopt(socket_handle, SOL_SOCKET, SO_BROADCAST,
                         (const char *)&broadcast_enabled, (mn_socklen)sizeof(broadcast_enabled));
    }
    memset(&address, 0, sizeof(address));
    address_length = (mn_socklen)sizeof(address);
    if (getsockname(socket_handle, (struct sockaddr *)&address, &address_length) == 0)
        instance->local_port = ntohs(address.sin_port);
    return socket_handle;
}

static void mn_close_current_socket(mn_instance *instance) {
    if (instance->socket != MN_BAD_SOCKET) mn_close_socket(instance->socket);
    instance->socket = MN_BAD_SOCKET;
    instance->local_port = 0;
}

static int mn_same_address(const struct sockaddr_in *left,
                           const struct sockaddr_in *right) {
    return left->sin_addr.s_addr == right->sin_addr.s_addr &&
           left->sin_port == right->sin_port;
}

static mn_peer *mn_find_peer_address(mn_instance *instance,
                                     const struct sockaddr_in *address) {
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active &&
            mn_same_address(&instance->peers[i].address, address))
            return &instance->peers[i];
    }
    return NULL;
}
static mn_peer *mn_find_peer_token(mn_instance *instance, uint64_t token) {
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active && instance->peers[i].token == token)
            return &instance->peers[i];
    }
    return NULL;
}
static mn_peer *mn_find_peer_id(mn_instance *instance, uint32_t player_id) {
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active && instance->peers[i].id == player_id)
            return &instance->peers[i];
    }
    return NULL;
}

static void mn_encode_player(uint8_t *out, const mn_player *player) {
    size_t offset = 0;
    size_t i;
    mn_write_u32(out + offset, player->id); offset += 4;
    mn_write_u32(out + offset, (uint32_t)player->score); offset += 4;
    mn_write_u32(out + offset, player->flags); offset += 4;
    mn_write_u32(out + offset, player->colour_rgba); offset += 4;
    for (i = 0; i < 4; ++i) { mn_write_u16(out + offset, player->appearance[i]); offset += 2; }
    memset(out + offset, 0, MN_PLAYER_NAME_LENGTH);
    mn_copy_text((char *)(out + offset), MN_PLAYER_NAME_LENGTH, player->name);
    offset += MN_PLAYER_NAME_LENGTH;
    mn_write_float(out + offset, player->position.x); offset += 4;
    mn_write_float(out + offset, player->position.y); offset += 4;
    mn_write_float(out + offset, player->position.z); offset += 4;
    mn_write_float(out + offset, player->rotation.x); offset += 4;
    mn_write_float(out + offset, player->rotation.y); offset += 4;
    mn_write_float(out + offset, player->rotation.z); offset += 4;
    mn_write_float(out + offset, player->rotation.w); offset += 4;
    mn_write_float(out + offset, player->velocity.x); offset += 4;
    mn_write_float(out + offset, player->velocity.y); offset += 4;
    mn_write_float(out + offset, player->velocity.z); offset += 4;
}

static int mn_float_is_valid(float value) {
    return isfinite((double)value) && fabs((double)value) <= 1000000.0;
}
static int mn_player_is_valid(const mn_player *player) {
    return mn_float_is_valid(player->position.x) &&
           mn_float_is_valid(player->position.y) &&
           mn_float_is_valid(player->position.z) &&
           mn_float_is_valid(player->rotation.x) &&
           mn_float_is_valid(player->rotation.y) &&
           mn_float_is_valid(player->rotation.z) &&
           mn_float_is_valid(player->rotation.w) &&
           mn_float_is_valid(player->velocity.x) &&
           mn_float_is_valid(player->velocity.y) &&
           mn_float_is_valid(player->velocity.z);
}
