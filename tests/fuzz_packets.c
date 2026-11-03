/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
/* Optional white-box libFuzzer harness for packet framing and player decoding.
   Build with Clang and MULTINET_BUILD_FUZZERS=ON. */
#include "../src/multinet.c"

#define MN_FUZZ_ROOM_ID 0x13572468u
#define MN_FUZZ_PEER_TOKEN UINT64_C(0x1122334455667788)

static void mn_fuzz_init_instance(mn_instance *instance, mn_role role,
                                  mn_status status,
                                  const struct sockaddr_in *peer_address) {
    mn_peer *peer;
    memset(instance, 0, sizeof(*instance));
    instance->socket = MN_BAD_SOCKET;
    instance->mdns_socket = MN_BAD_SOCKET;
    instance->ready = 1;
    instance->config.app_id = MN_DEFAULT_APP_ID;
    instance->config.heartbeat_seconds = 0.10f;
    instance->config.timeout_seconds = 8.0f;
    instance->info.app_id = MN_DEFAULT_APP_ID;
    instance->info.role = role;
    instance->info.status = status;
    instance->info.room_id = MN_FUZZ_ROOM_ID;
    instance->info.max_players = 2;
    instance->info.player_count = role == MN_ROLE_HOST ? 2u : 0u;
    instance->next_player_id = 3u;
    mn_player_state_limits_init(&instance->state_limits);

    if (role != MN_ROLE_HOST || !peer_address) return;
    instance->local_player.rotation.w = 1.0f;
    mn_copy_text(instance->local_player.name,
                 sizeof(instance->local_player.name), "FuzzHost");
    peer = &instance->peers[0];
    peer->active = 1;
    peer->id = 2u;
    peer->token = MN_FUZZ_PEER_TOKEN;
    peer->address = *peer_address;
    peer->player.id = peer->id;
    peer->player.rotation.w = 1.0f;
    mn_copy_text(peer->player.name, sizeof(peer->player.name), "FuzzPeer");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mn_instance framing_instance;
    mn_instance state_instance;
    struct sockaddr_in from;
    uint8_t offer_payload[MN_OFFER_SIZE];
    uint8_t offer_packet[MN_HEADER_SIZE + MN_OFFER_SIZE];
    uint8_t state_payload[12u + MN_WIRE_PLAYER_SIZE];
    uint8_t state_packet[MN_HEADER_SIZE + sizeof(state_payload)];
    size_t copied;
    size_t offer_copied;
    size_t offer_packet_size;
    size_t packet_size;

    if (size > MN_MAX_DATAGRAM_SIZE) return 0;
    memset(&from, 0, sizeof(from));
    from.sin_family = AF_INET;
    from.sin_port = htons(54321u);
    from.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* Pass the original bytes through packet framing and offer parsing. */
    mn_fuzz_init_instance(&framing_instance, MN_ROLE_NONE,
                          MN_STATUS_SEARCHING, NULL);
    mn_handle_packet(&framing_instance, data, size, &from);

    /* Preserve a valid offer envelope while mutating its room and host names.
       This covers the offer parser even when raw input has a broken header. */
    memset(offer_payload, 0, sizeof(offer_payload));
    offer_payload[0] = 1u;
    offer_payload[1] = 2u;
    offer_copied = size < sizeof(offer_payload) - 2u ?
        size : sizeof(offer_payload) - 2u;
    if (offer_copied != 0u && data != NULL)
        memcpy(offer_payload + 2u, data, offer_copied);
    offer_packet_size = mn_make_packet(offer_packet, MN_PKT_OFFER,
                                       MN_DEFAULT_APP_ID, MN_FUZZ_ROOM_ID,
                                       offer_payload, sizeof(offer_payload));
    mn_handle_packet(&framing_instance, offer_packet, offer_packet_size, &from);

    /* Wrap arbitrary bytes in a valid authenticated state envelope. This
       reliably reaches the player decoder instead of requiring the fuzzer to
       rediscover the protocol magic, app ID, room ID and session token. */
    mn_fuzz_init_instance(&state_instance, MN_ROLE_HOST,
                          MN_STATUS_HOSTING, &from);
    mn_write_u64(state_payload, MN_FUZZ_PEER_TOKEN);
    mn_write_u32(state_payload + 8u, 1u);
    memset(state_payload + 12u, 0, MN_WIRE_PLAYER_SIZE);
    copied = size < MN_WIRE_PLAYER_SIZE ? size : MN_WIRE_PLAYER_SIZE;
    if (copied != 0u && data != NULL)
        memcpy(state_payload + 12u, data, copied);
    packet_size = mn_make_packet(state_packet, MN_PKT_STATE,
                                 MN_DEFAULT_APP_ID, MN_FUZZ_ROOM_ID,
                                 state_payload, sizeof(state_payload));
    mn_handle_packet(&state_instance, state_packet, packet_size, &from);
    return 0;
}
