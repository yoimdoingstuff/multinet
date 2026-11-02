
            mn_send_welcome(instance, peer);
            mn_broadcast_snapshot(instance);
            return;
        }
        if (room_id != instance->info.room_id) return;
        if (type == MN_PKT_STATE && payload_size == 8u + MN_WIRE_PLAYER_SIZE) {
            uint64_t token = mn_read_u64(payload);
            mn_peer *peer = mn_find_peer_token(instance, token);
            mn_player player;
            if (!peer || !mn_same_address(&peer->address, from)) return;
            if (!mn_decode_player(&player, payload + 8)) return;
            peer->silence = 0.0f;
            player.id = peer->id;
            mn_copy_text(player.name, sizeof(player.name), peer->player.name);
            peer->player = player;
            return;
        }
        if (type == MN_PKT_LEAVE && payload_size == 8u) {
            mn_peer *peer = mn_find_peer_token(instance, mn_read_u64(payload));
            if (peer && mn_same_address(&peer->address, from)) {
                mn_remove_peer(instance, peer, "Player left the room");
                mn_broadcast_snapshot(instance);
            }
            return;
        }
        if (type == MN_PKT_MESSAGE) {
            mn_handle_host_message(instance, payload, payload_size, from);
            return;
        }
        return;
    }

    if (instance->info.role == MN_ROLE_CLIENT &&
        mn_same_address(from, &instance->server)) {
        instance->server_silence = 0.0f;
        if (room_id != instance->info.room_id &&
            !(type == MN_PKT_WELCOME && instance->info.status == MN_STATUS_CONNECTING)) return;
        if (type == MN_PKT_WELCOME && payload_size == MN_WELCOME_SIZE &&
            instance->info.status == MN_STATUS_CONNECTING) {
            instance->info.local_player_id = mn_read_u32(payload);
            instance->session_token = mn_read_u64(payload + 4);
            instance->info.max_players = payload[12];
            instance->info.room_id = room_id;
            memcpy(instance->info.room_name, payload + 13, MN_ROOM_NAME_LENGTH);
            instance->info.room_name[MN_ROOM_NAME_LENGTH - 1] = '\0';
            instance->local_player.id = instance->info.local_player_id;
            mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name),
                         instance->info.local_name);
            instance->players[0] = instance->local_player;
            instance->info.status = MN_STATUS_CONNECTED;
            instance->info.player_count = 1;
            mn_emit_simple_event(instance, MN_EVENT_CONNECTED, "Connected to room",
                                 instance->info.local_player_id);
            return;
        }
        if (type == MN_PKT_START && payload_size == 0) {
            if (!instance->info.match_started) {
                instance->info.match_started = 1;
                mn_emit_simple_event(instance, MN_EVENT_MATCH_STARTED,
                                     "Host started the match", 0);
            }
            return;
        }
        if (type == MN_PKT_SNAPSHOT && payload_size >= 2u) {
            mn_player incoming[MN_MAX_PLAYERS];
            size_t count = payload[0], i, expected;
            int was_started = instance->info.match_started;
            if (count == 0 || count > MN_MAX_PLAYERS) return;
            expected = 2u + count * MN_WIRE_PLAYER_SIZE;
            if (payload_size != expected || payload[1] > 1u) return;
            memset(incoming, 0, sizeof(incoming));
            for (i = 0; i < count; ++i) {
                if (!mn_decode_player(&incoming[i], payload + 2u + i * MN_WIRE_PLAYER_SIZE))
                    return;
            }
