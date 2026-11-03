/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet.h"
#include "../src/multinet_crypto.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned int ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(unsigned int ms) {
    struct timespec delay;
    delay.tv_sec = (time_t)(ms / 1000u);
    delay.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}
#endif

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        result = 1; goto cleanup; } \
} while (0)

typedef struct test_state_validator_context {
    unsigned calls;
    unsigned rejected_score_regressions;
    unsigned out_of_bounds_candidates;
} test_state_validator_context;

static int test_aes256gcm_vector(void) {
    static const uint8_t key[32] = {0};
    static const uint8_t nonce[12] = {0};
    static const uint8_t plaintext[16] = {0};
    static const uint8_t expected_ciphertext[16] = {
        0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
        0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18
    };
    static const uint8_t expected_tag[16] = {
        0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
        0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19
    };
    uint8_t ciphertext[16], tag[16], recovered[16], bad_tag[16];
    if (!mn_crypto_aes256gcm_encrypt(key, nonce, NULL, 0,
                                     plaintext, sizeof(plaintext),
                                     ciphertext, tag))
        return 0;
    if (memcmp(ciphertext, expected_ciphertext, sizeof(ciphertext)) != 0 ||
        memcmp(tag, expected_tag, sizeof(tag)) != 0)
        return 0;
    if (!mn_crypto_aes256gcm_decrypt(key, nonce, NULL, 0,
                                     ciphertext, sizeof(ciphertext), tag,
                                     recovered) ||
        memcmp(recovered, plaintext, sizeof(plaintext)) != 0)
        return 0;
    memcpy(bad_tag, tag, sizeof(tag));
    bad_tag[0] ^= 0x01u;
    return !mn_crypto_aes256gcm_decrypt(key, nonce, NULL, 0,
                                        ciphertext, sizeof(ciphertext), bad_tag,
                                        recovered);
}

static int test_hmac_sha256_vector(void) {
    static const uint8_t expected[32] = {
        0xf7, 0xbc, 0x83, 0xf4, 0x30, 0x53, 0x84, 0x24,
        0xb1, 0x32, 0x98, 0xe6, 0xaa, 0x6f, 0xb1, 0x43,
        0xef, 0x4d, 0x59, 0xa1, 0x49, 0x46, 0x17, 0x59,
        0x97, 0x47, 0x9d, 0xbc, 0x2d, 0x1a, 0x3c, 0xd8
    };
    static const uint8_t key[] = "key";
    static const uint8_t message[] = "The quick brown fox jumps over the lazy dog";
    uint8_t actual[32];
    return mn_crypto_hmac_sha256(key, sizeof(key) - 1u, message,
                                 sizeof(message) - 1u, actual) &&
           memcmp(actual, expected, sizeof(expected)) == 0;
}

static int test_bytes_nonzero(const uint8_t *bytes, size_t size) {
    size_t i;
    uint8_t value = 0;
    for (i = 0; i < size; ++i) value |= bytes[i];
    return value != 0;
}

static int test_player_state_validator(const mn_player *previous,
                                       const mn_player *incoming,
                                       void *user_data) {
    test_state_validator_context *context =
        (test_state_validator_context *)user_data;
    if (!previous || !incoming || !context) return 0;
    context->calls++;
    if (incoming->position.x > 100.0f || incoming->position.x < -100.0f ||
        incoming->position.y > 100.0f || incoming->position.y < -100.0f ||
        incoming->position.z > 100.0f || incoming->position.z < -100.0f ||
        incoming->velocity.x > 20.0f || incoming->velocity.x < -20.0f ||
        incoming->velocity.y > 20.0f || incoming->velocity.y < -20.0f ||
        incoming->velocity.z > 20.0f || incoming->velocity.z < -20.0f) {
        context->out_of_bounds_candidates++;
        return 0;
    }
    if (incoming->score < previous->score) {
        context->rejected_score_regressions++;
        return 0;
    }
    return 1;
}

int main(void) {
    mn_config config;
    mn_instance *host = NULL;
    mn_instance *client = NULL;
    mn_instance *scanner = NULL;
    mn_instance *auto_scanner = NULL;
    mn_instance *bad_key_scanner = NULL;
    mn_instance *rejected_client = NULL;
    mn_instance *spoof_client = NULL;
    mn_config auto_config;
    mn_info auto_info;
    mn_room discovered_rooms[MN_MAX_ROOMS];
    size_t discovered_count = 0;
    mn_instance *extra_clients[MN_MAX_PLAYERS - 2u] = {0};
    mn_interface interfaces[64];
    mn_interface loopback_interface;
    mn_info scanner_info;
    size_t interface_count, interface_index;
    int found_loopback = 0;
    mn_info host_info, client_info, rejected_info;
    mn_player state, received;
    mn_player_state_limits state_limits, invalid_state_limits;
    test_state_validator_context validator_context = {0u, 0u, 0u};
    uint8_t room_key[MN_ROOM_KEY_BYTES], wrong_room_key[MN_ROOM_KEY_BYTES];
    uint8_t client_device_id[MN_DEVICE_ID_BYTES], rejected_device_id[MN_DEVICE_ID_BYTES];
    uint8_t client_device_secret[MN_DEVICE_SECRET_BYTES];
    uint8_t spoof_device_secret[MN_DEVICE_SECRET_BYTES];
    uint8_t stored_pair_secret[MN_DEVICE_SECRET_BYTES];
    uint8_t restored_device_id[MN_DEVICE_ID_BYTES];
    uint8_t restored_device_secret[MN_DEVICE_SECRET_BYTES];
    int pairing_request_seen = 0, rejection_request_seen = 0, rejection_received = 0;
    int spoof_rejected = 0;
    mn_event event;
    int result = 0;
    int connected = 0, message_received = 0, host_message_received = 0;
    int reliable_received = 0, reliable_host_received = 0;
    int client_reliable_acked = 0, host_reliable_acked = 0;
    int reliable_failed_on_leave = 0;
    uint32_t client_reliable_id = 0, host_reliable_id = 0;
    uint32_t cancelled_delivery_id = 0;
    int client_ping_received = 0, host_ping_received = 0;
    unsigned iteration;
    size_t extra_count = 0, j;
    mn_config_init(&config);
    config.port = 38927;
    config.enable_broadcast = 0;
    host = mn_create(&config);
    client = mn_create(&config);
    scanner = mn_create(&config);
    bad_key_scanner = mn_create(&config);
    auto_config = config;
    auto_config.enable_broadcast = 1;
    auto_scanner = mn_create(&auto_config);
    CHECK(host != NULL && client != NULL && scanner != NULL &&
          bad_key_scanner != NULL && auto_scanner != NULL,
          "create independent host, client and discovery instances");
    CHECK(mn_crypto_available() && test_aes256gcm_vector() &&
          test_hmac_sha256_vector() && mn_generate_room_key(room_key) &&
          mn_generate_room_key(wrong_room_key),
          "AES-GCM/HMAC backend and operating-system key generation pass known-answer tests");
    wrong_room_key[0] ^= 0xA5u;
    CHECK(mn_set_room_key(host, room_key) &&
          mn_set_room_key(client, room_key) &&
          mn_set_room_key(scanner, room_key) &&
          mn_set_room_key(auto_scanner, room_key) &&
          mn_set_room_key(bad_key_scanner, wrong_room_key),
          "configure authenticated encryption before opening the room");
    CHECK(mn_get_device_id(client, client_device_id) &&
          mn_get_device_secret(client, client_device_secret) &&
          test_bytes_nonzero(client_device_id, MN_DEVICE_ID_BYTES) &&
          test_bytes_nonzero(client_device_secret, MN_DEVICE_SECRET_BYTES),
          "client receives a non-zero persistent device ID and private secret");
    mn_player_state_limits_init(&state_limits);
    state_limits.max_abs_position = 100.0f;
    state_limits.max_abs_velocity = 20.0f;
    invalid_state_limits = state_limits;
    invalid_state_limits.max_abs_velocity = -1.0f;
    CHECK(!mn_set_player_state_validation(host, &invalid_state_limits,
                                          test_player_state_validator,
                                          &validator_context),
          "reject invalid negative remote-state limits");
    CHECK(mn_set_player_state_validation(host, &state_limits,
                                         test_player_state_validator,
                                         &validator_context),
          "configure host-side bounds and an application state validator");
    interface_count = mn_get_interfaces(interfaces, 64);
    CHECK(interface_count > 0, "enumerate local IPv4 network interfaces");
    for (interface_index = 0; interface_index < interface_count && interface_index < 64;
         ++interface_index) {
        if (interfaces[interface_index].type == MN_INTERFACE_LOOPBACK &&
            strcmp(interfaces[interface_index].address, "127.0.0.1") == 0) {
            loopback_interface = interfaces[interface_index];
            found_loopback = 1;
            break;
        }
    }
    CHECK(found_loopback, "enumerate the IPv4 loopback interface");

    CHECK(!mn_allow_join_ipv4(host, "not-an-ip-address"),
          "reject malformed IPv4 addresses in the join allow-list");
    CHECK(mn_allow_join_ipv4(host, "192.0.2.1") &&
          mn_allow_join_ipv4(host, "192.0.2.1"),
          "add an allowed IPv4 address idempotently");
    CHECK(mn_set_join_allowlist_enabled(host, 1),
          "enable the host join allow-list");
    CHECK(mn_set_pairing_required(host, 1),
          "require device pairing before admitting room members");

    CHECK(mn_discover_auto(auto_scanner),
          "start combined broadcast, directed-subnet and mDNS discovery");
    mn_get_info(auto_scanner, &auto_info);
    CHECK(auto_info.status == MN_STATUS_SEARCHING,
          "automatic discovery enters the searching state");
    for (iteration = 0; iteration < 12; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(auto_scanner, 0.01f);
        sleep_ms(5);
    }
    mn_stop_discovery(auto_scanner);
    mn_get_info(auto_scanner, &auto_info);
    CHECK(auto_info.status == MN_STATUS_OFFLINE,
          "automatic discovery can be stopped cleanly");

    CHECK(mn_host_on(host, "Host", "Local Test", MN_MAX_PLAYERS,
                     loopback_interface.address),
          "host explicitly on the selected loopback interface");
    CHECK(!mn_set_room_key(host, room_key),
          "prevent changing the room key while hosting");
    CHECK(mn_discover_at(bad_key_scanner, "127.0.0.1", config.port),
          "wrong-key client can attempt unicast discovery");
    for (iteration = 0; iteration < 20; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(bad_key_scanner, 0.01f);
        sleep_ms(5);
    }
    CHECK(mn_get_rooms(bad_key_scanner, NULL, 0) == 0,
          "wrong room key cannot authenticate or discover the encrypted room");
    mn_stop_discovery(bad_key_scanner);

    CHECK(!mn_discover(scanner),
          "disabled broadcast reports that an alternate discovery path is required");
    CHECK(mn_discover_at(scanner, "127.0.0.1", config.port),
          "start direct unicast discovery when broadcast is disabled");
    for (iteration = 0; iteration < 200; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(scanner, 0.01f);
        discovered_count = mn_get_rooms(scanner, discovered_rooms, MN_MAX_ROOMS);
        if (discovered_count > 0) break;
        sleep_ms(5);
    }
    if (discovered_count != 1) {
        mn_get_info(scanner, &scanner_info);
        fprintf(stderr, "DIAG: secure discovery rooms=%u status=%u last_error='%s'\n",
                (unsigned)discovered_count, (unsigned)scanner_info.status,
                scanner_info.last_error);
    }
    CHECK(discovered_count == 1 &&
          strcmp(discovered_rooms[0].name, "Local Test") == 0,
          "unicast discovery finds room without LAN broadcast");

    mn_get_info(scanner, &scanner_info);
    if (scanner_info.mdns_available) {
        CHECK(mn_discover_mdns(scanner), "start DNS-SD/mDNS room discovery");
        for (iteration = 0; iteration < 40; ++iteration) {
            mn_update(host, 0.01f);
            mn_update(scanner, 0.01f);
            (void)mn_get_rooms(scanner, discovered_rooms, MN_MAX_ROOMS);
            sleep_ms(5);
        }
        mn_stop_discovery(scanner);
        mn_get_info(scanner, &scanner_info);
        CHECK(scanner_info.status == MN_STATUS_OFFLINE,
              "mDNS discovery can be stopped cleanly");
    } else {
        CHECK(!mn_discover_mdns(scanner),
              "mDNS reports unavailable cleanly while direct IP fallback remains usable");
    }

    CHECK(mn_join_from(client, loopback_interface.address, "127.0.0.1",
                       config.port, "Client"),
          "join while binding the client to a selected local interface");
    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        sleep_ms(5);
    }
    mn_get_info(host, &host_info);
    mn_get_info(client, &client_info);
    CHECK(host_info.player_count == 1 &&
          client_info.status == MN_STATUS_CONNECTING,
          "host allow-list blocks a new client from an unapproved address");
    CHECK(mn_disallow_join_ipv4(host, "192.0.2.1"),
          "remove an address from the host allow-list");
    CHECK(mn_allow_join_ipv4(host, loopback_interface.address),
          "approve the client's actual source IPv4 address");
    CHECK(!mn_set_join_allowlist_enabled(host, 2),
          "reject non-boolean allow-list enable values");

    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        while (mn_poll_event(host, &event)) {
            if (event.type == MN_EVENT_PAIRING_REQUEST &&
                memcmp(event.device_id, client_device_id, MN_DEVICE_ID_BYTES) == 0)
                pairing_request_seen = 1;
        }
        sleep_ms(5);
    }
    mn_get_info(host, &host_info);
    CHECK(pairing_request_seen && host_info.player_count == 1,
          "unknown device creates a pairing request without entering the room");
    CHECK(mn_approve_pairing(host, client_device_id, 1),
          "approve pairing and add the device to the trusted-device list");

    memset(&state, 0, sizeof(state));
    state.position.x = 12.5f;
    state.position.y = -3.25f;
    state.rotation.w = 1.0f;
    state.velocity.x = 1.0f;
    state.colour_rgba = 0xFF8844FFu;
    state.score = 7;
    state.appearance[0] = 12;

    for (iteration = 0; iteration < 500; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        mn_get_info(client, &client_info);
        if (client_info.status == MN_STATUS_CONNECTED) { connected = 1; break; }
        sleep_ms(5);
    }
    CHECK(connected, "paired client completes HMAC challenge and receives welcome packet");
    CHECK(mn_get_device_id(client, rejected_device_id) &&
          memcmp(client_device_id, rejected_device_id, MN_DEVICE_ID_BYTES) == 0 &&
          mn_get_paired_device_secret(host, client_device_id, stored_pair_secret) &&
          memcmp(stored_pair_secret, client_device_secret, MN_DEVICE_SECRET_BYTES) == 0,
          "host stores the approved device secret against its persistent ID");

    spoof_client = mn_create(&config);
    CHECK(spoof_client != NULL &&
          mn_get_device_secret(spoof_client, spoof_device_secret) &&
          mn_set_room_key(spoof_client, room_key) &&
          mn_set_device_identity(spoof_client, client_device_id, spoof_device_secret) &&
          mn_join_from(spoof_client, loopback_interface.address,
                       "127.0.0.1", config.port, "Spoof"),
          "client with a cloned public ID but a different private secret attempts to join");
    for (iteration = 0; iteration < 40 && !spoof_rejected; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(spoof_client, 0.01f);
        while (mn_poll_event(spoof_client, &event)) {
            if (event.type == MN_EVENT_PAIRING_REJECTED) spoof_rejected = 1;
        }
        sleep_ms(5);
    }
    mn_get_info(spoof_client, &rejected_info);
    CHECK(spoof_rejected && rejected_info.status == MN_STATUS_OFFLINE,
          "host refuses a cloned device ID with the wrong private secret");
    CHECK(mn_set_device_identity(spoof_client, client_device_id,
                                 client_device_secret) &&
          mn_get_device_id(spoof_client, restored_device_id) &&
          mn_get_device_secret(spoof_client, restored_device_secret) &&
          memcmp(restored_device_id, client_device_id, MN_DEVICE_ID_BYTES) == 0 &&
          memcmp(restored_device_secret, client_device_secret,
                 MN_DEVICE_SECRET_BYTES) == 0,
          "application can restore the original device identity after restart");

    rejected_client = mn_create(&config);
    CHECK(rejected_client != NULL &&
          mn_set_room_key(rejected_client, room_key) &&
          mn_join_from(rejected_client, loopback_interface.address,
                       "127.0.0.1", config.port, "Rejected"),
          "second device begins an encrypted pairing request");
    CHECK(mn_get_device_id(rejected_client, rejected_device_id),
          "read the second device identifier");
    for (iteration = 0; iteration < 80 && !rejection_request_seen; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(rejected_client, 0.01f);
        while (mn_poll_event(host, &event)) {
            if (event.type == MN_EVENT_PAIRING_REQUEST &&
                memcmp(event.device_id, rejected_device_id, MN_DEVICE_ID_BYTES) == 0)
                rejection_request_seen = 1;
        }
        sleep_ms(5);
    }
    CHECK(rejection_request_seen,
          "host receives a separate pairing request for an unknown device");
    CHECK(mn_approve_pairing(host, rejected_device_id, 0),
          "host can reject a device's pairing request");
    for (iteration = 0; iteration < 20 && !rejection_received; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(rejected_client, 0.01f);
        while (mn_poll_event(rejected_client, &event)) {
            if (event.type == MN_EVENT_PAIRING_REJECTED)
                rejection_received = 1;
        }
        sleep_ms(5);
    }
    mn_get_info(client, &client_info);
    mn_get_info(rejected_client, &rejected_info);
    CHECK(rejection_received && mn_is_connected(client) &&
          rejected_info.status == MN_STATUS_OFFLINE,
          "rejected device is notified without disturbing the approved client");
    CHECK(mn_unpair_device(host, client_device_id) &&
          mn_pair_device(host, client_device_id, client_device_secret) &&
          mn_get_paired_device_secret(host, client_device_id, stored_pair_secret) &&
          memcmp(stored_pair_secret, client_device_secret,
                 MN_DEVICE_SECRET_BYTES) == 0,
          "host can revoke, re-pair and restore the stored device credential");
    CHECK(mn_set_pairing_required(host, 0),
          "host can switch off approval prompts for this open test room");

    for (iteration = 0; iteration < 80; ++iteration) {
        (void)mn_set_local_player(client, &state);
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        sleep_ms(5);
    }
    mn_get_info(host, &host_info);
    mn_get_info(client, &client_info);
    CHECK(host_info.player_count == 2 && client_info.player_count == 2,
          "both instances receive the two-player roster");
    CHECK(mn_get_player(host, client_info.local_player_id, &received),
          "host can look up remote player by ID");
    CHECK(received.position.x == 12.5f && received.position.y == -3.25f &&
          received.score == 7 && received.appearance[0] == 12,
          "player snapshot fields survive wire serialisation");
    state.position.x = 101.0f;
    CHECK(mn_set_local_player(client, &state),
          "client can stage a position outside the host's stricter game bounds");
    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        sleep_ms(5);
    }
    CHECK(mn_get_player(host, client_info.local_player_id, &received) &&
          received.position.x == 12.5f && received.score == 7,
          "host rejects remote positions beyond the configured absolute limit");

    state.position.x = 12.5f;
    state.velocity.x = 21.0f;
    CHECK(mn_set_local_player(client, &state),
          "client can stage a velocity outside the host's stricter game bounds");
    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        sleep_ms(5);
    }
    CHECK(mn_get_player(host, client_info.local_player_id, &received) &&
          received.position.x == 12.5f && received.velocity.x == 1.0f &&
          received.score == 7,
          "host rejects remote velocity beyond the configured absolute limit");
    CHECK(validator_context.out_of_bounds_candidates == 0u,
          "application callback never sees states rejected by built-in bounds");

    state.position.x = 13.0f;
    state.velocity.x = 1.0f;
    state.score = 6;
    CHECK(mn_set_local_player(client, &state),
          "client can stage an application-invalid score update");
    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        sleep_ms(5);
    }
    CHECK(mn_get_player(host, client_info.local_player_id, &received) &&
          received.position.x == 12.5f && received.score == 7 &&
          validator_context.calls > 0u &&
          validator_context.rejected_score_regressions > 0u,
          "application callback rejects score regression without replacing accepted state");

    state.score = 8;
    CHECK(mn_set_local_player(client, &state),
          "client stages a valid state after a rejected update");
    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        sleep_ms(5);
    }
    CHECK(mn_get_player(host, client_info.local_player_id, &received) &&
          received.position.x == 13.0f && received.score == 8,
          "host accepts later state that passes bounds and application validation");

    CHECK(mn_send_message(client, 0, 42, "client-to-host", 14),
          "client sends an application message");
    for (iteration = 0; iteration < 20 && !message_received; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        while (mn_poll_event(host, &event)) {
            if (event.type == MN_EVENT_MESSAGE && event.channel == 42 &&
                event.data_size == 14 && memcmp(event.data, "client-to-host", 14) == 0)
                message_received = 1;
        }
        sleep_ms(5);
    }
    CHECK(message_received, "host receives client application message");

    mn_get_info(client, &client_info);
    CHECK(mn_send_message(host, client_info.local_player_id, 77, "host-reply", 10),
          "host sends a targeted application message");
    for (iteration = 0; iteration < 20 && !host_message_received; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        while (mn_poll_event(client, &event)) {
            if (event.type == MN_EVENT_MESSAGE && event.channel == 77 &&
                event.data_size == 10 && memcmp(event.data, "host-reply", 10) == 0)
                host_message_received = 1;
        }
        sleep_ms(5);
    }
    CHECK(host_message_received, "client receives targeted host message");


    {
        static const char reliable_text[] = "reliable-client";
        CHECK(mn_send_reliable_message_ex(client, 0, 88, reliable_text,
                                          sizeof(reliable_text) - 1,
                                          &client_reliable_id) &&
              client_reliable_id != 0,
              "client queues a reliable message and receives its delivery ID");
        for (iteration = 0; iteration < 60 && !reliable_received; ++iteration) {
            mn_update(client, 0.01f);
            mn_update(host, 0.01f);
            while (mn_poll_event(host, &event)) {
                if (event.type == MN_EVENT_MESSAGE && event.channel == 88 &&
                    event.data_size == sizeof(reliable_text) - 1 &&
                    memcmp(event.data, reliable_text, sizeof(reliable_text) - 1) == 0)
                    reliable_received = 1;
            }
            sleep_ms(5);
        }
    }
    CHECK(reliable_received, "host receives client reliable message");

    for (iteration = 0; iteration < 60 && !client_reliable_acked; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        while (mn_poll_event(client, &event)) {
            if (event.type == MN_EVENT_MESSAGE_ACKNOWLEDGED &&
                event.delivery_id == client_reliable_id && event.player_id == 0)
                client_reliable_acked = 1;
        }
        sleep_ms(5);
    }
    CHECK(client_reliable_acked,
          "client receives an acknowledgement event for its delivery ID");

    {
        static const char reliable_text[] = "reliable-host";
        mn_get_info(client, &client_info);
        CHECK(mn_send_reliable_message_ex(host, client_info.local_player_id, 99,
                                          reliable_text, sizeof(reliable_text) - 1,
                                          &host_reliable_id) && host_reliable_id != 0,
              "host queues a targeted reliable message and receives its delivery ID");
        for (iteration = 0; iteration < 60 && !reliable_host_received; ++iteration) {
            mn_update(host, 0.01f);
            mn_update(client, 0.01f);
            while (mn_poll_event(client, &event)) {
                if (event.type == MN_EVENT_MESSAGE && event.channel == 99 &&
                    event.data_size == sizeof(reliable_text) - 1 &&
                    memcmp(event.data, reliable_text, sizeof(reliable_text) - 1) == 0)
                    reliable_host_received = 1;
            }
            sleep_ms(5);
        }
    }
    CHECK(reliable_host_received, "client receives host reliable message");

    for (iteration = 0; iteration < 60 && !host_reliable_acked; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        while (mn_poll_event(host, &event)) {
            if (event.type == MN_EVENT_MESSAGE_ACKNOWLEDGED &&
                event.delivery_id == host_reliable_id &&
                event.player_id == client_info.local_player_id)
                host_reliable_acked = 1;
        }
        sleep_ms(5);
    }
    CHECK(host_reliable_acked,
          "host receives a per-player acknowledgement event for its delivery ID");

    CHECK(mn_ping(client, 0), "client starts a ping to the host");
    for (iteration = 0; iteration < 60 && !client_ping_received; ++iteration) {
        mn_update(client, 0.01f);
        mn_update(host, 0.01f);
        mn_get_info(client, &client_info);
        if (client_info.ping_available) client_ping_received = 1;
        sleep_ms(5);
    }
    CHECK(client_ping_received && client_info.ping_ms >= 0.0f &&
          client_info.ping_ms < 5000.0f,
          "client receives a valid host round-trip latency measurement");

    mn_get_info(client, &client_info);
    CHECK(mn_ping(host, client_info.local_player_id),
          "host starts a ping to a selected client");
    for (iteration = 0; iteration < 60 && !host_ping_received; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        mn_get_info(host, &host_info);
        if (host_info.ping_available) host_ping_received = 1;
        sleep_ms(5);
    }
    CHECK(host_ping_received && host_info.ping_ms >= 0.0f &&
          host_info.ping_ms < 5000.0f,
          "host receives a valid client round-trip latency measurement");

    CHECK(mn_start_match(host), "host starts match");
    for (iteration = 0; iteration < 10; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        sleep_ms(5);
    }


    mn_get_info(client, &client_info);
    CHECK(client_info.match_started != 0, "client receives match-start event");

    /* Fill the room to its advertised 16-player cap, using separate instances
       in this process to exercise independent client sockets and host slots. */
    for (j = 0; j < MN_MAX_PLAYERS - 2u; ++j) {
        char name[MN_PLAYER_NAME_LENGTH];
        extra_clients[j] = mn_create(&config);
        CHECK(extra_clients[j] != NULL &&
              mn_set_room_key(extra_clients[j], room_key),
              "create additional encrypted player instance");
        snprintf(name, sizeof(name), "Guest%02u", (unsigned)(j + 1u));
        if (j == 0) {
            CHECK(mn_join_room(extra_clients[j], &discovered_rooms[0], name),
                  "join a discovered room through the room convenience API");
        } else {
            CHECK(mn_join(extra_clients[j], "127.0.0.1", config.port, name),
                  "additional client starts joining by manual IPv4 address");
        }
        extra_count++;
    }
    for (iteration = 0; iteration < 1000; ++iteration) {
        int all_clients_connected = 1;
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        for (j = 0; j < extra_count; ++j) {
            mn_update(extra_clients[j], 0.01f);
            mn_get_info(extra_clients[j], &client_info);
            if (client_info.status != MN_STATUS_CONNECTED)
                all_clients_connected = 0;
        }
        mn_get_info(host, &host_info);
        if (host_info.player_count == MN_MAX_PLAYERS && all_clients_connected)
            break;
        sleep_ms(4);
    }
    mn_get_info(host, &host_info);
    if (host_info.player_count != MN_MAX_PLAYERS) {
        fprintf(stderr, "DIAG: host player_count=%u, extra_clients=%u\n",
                (unsigned)host_info.player_count, (unsigned)extra_count);
        for (j = 0; j < extra_count; ++j) {
            mn_get_info(extra_clients[j], &client_info);
            fprintf(stderr, "DIAG: extra[%u] status=%u\n",
                    (unsigned)j, (unsigned)client_info.status);
        }
    }
    CHECK(host_info.player_count == MN_MAX_PLAYERS,
          "host supports 16 simultaneous players including the host");
    for (j = 0; j < extra_count; ++j) {
        mn_get_info(extra_clients[j], &client_info);
        CHECK(client_info.status == MN_STATUS_CONNECTED,
              "all additional clients connect");
    }

    {
        static const char broadcast_text[] = "reliable-room-broadcast";
        uint32_t broadcast_delivery_id = 0;
        uint32_t acknowledged_mask = 0;
        uint32_t expected_mask = 0;
        int client_received[MN_MAX_PLAYERS] = {0};
        size_t received_count = 0;
        size_t acknowledged_count = 0;
        unsigned player_id;
        for (player_id = 2; player_id <= MN_MAX_PLAYERS; ++player_id)
            expected_mask |= (uint32_t)1u << player_id;

        CHECK(mn_send_reliable_message_ex(host, 0, 113, broadcast_text,
                                          sizeof(broadcast_text) - 1,
                                          &broadcast_delivery_id) &&
              broadcast_delivery_id != 0,
              "host queues a reliable broadcast to every remote player");
        for (iteration = 0; iteration < 120 && acknowledged_count < MN_MAX_PLAYERS - 1u;
             ++iteration) {
            mn_update(host, 0.01f);
            mn_update(client, 0.01f);
            while (mn_poll_event(client, &event)) {
                if (event.type == MN_EVENT_MESSAGE && event.channel == 113 &&
                    event.data_size == sizeof(broadcast_text) - 1 &&
                    memcmp(event.data, broadcast_text, sizeof(broadcast_text) - 1) == 0 &&
                    !client_received[0]) {
                    client_received[0] = 1;
                    received_count++;
                }
            }
            for (j = 0; j < extra_count; ++j) {
                mn_update(extra_clients[j], 0.01f);
                while (mn_poll_event(extra_clients[j], &event)) {
                    if (event.type == MN_EVENT_MESSAGE && event.channel == 113 &&
                        event.data_size == sizeof(broadcast_text) - 1 &&
                        memcmp(event.data, broadcast_text, sizeof(broadcast_text) - 1) == 0 &&
                        !client_received[j + 1u]) {
                        client_received[j + 1u] = 1;
                        received_count++;
                    }
                }
            }
            while (mn_poll_event(host, &event)) {
                if (event.type == MN_EVENT_MESSAGE_ACKNOWLEDGED &&
                    event.delivery_id == broadcast_delivery_id &&
                    event.player_id >= 2u && event.player_id <= MN_MAX_PLAYERS) {
                    uint32_t bit = (uint32_t)1u << event.player_id;
                    if ((acknowledged_mask & bit) == 0) {
                        acknowledged_mask |= bit;
                        acknowledged_count++;
                    }
                }
            }
            sleep_ms(5);
        }
        CHECK(received_count == MN_MAX_PLAYERS - 1u,
              "all 15 remote clients receive the reliable room broadcast");
        CHECK(acknowledged_count == MN_MAX_PLAYERS - 1u &&
              acknowledged_mask == expected_mask,
              "host receives one matching acknowledgement from every remote player");
    }

    for (j = 0; j < extra_count; ++j) mn_leave(extra_clients[j]);
    mn_get_info(client, &client_info);
    CHECK(mn_send_reliable_message_ex(host, client_info.local_player_id, 112,
                                      "cancel-on-leave", 15,
                                      &cancelled_delivery_id) &&
          cancelled_delivery_id != 0,
          "host queues a reliable delivery before a peer leaves");
    mn_leave(client);
    for (iteration = 0; iteration < 30 && !reliable_failed_on_leave; ++iteration) {
        mn_update(host, 0.01f);
        while (mn_poll_event(host, &event)) {
            if (event.type == MN_EVENT_MESSAGE_FAILED &&
                event.delivery_id == cancelled_delivery_id &&
                event.player_id == client_info.local_player_id)
                reliable_failed_on_leave = 1;
        }
        sleep_ms(5);
    }
    CHECK(reliable_failed_on_leave,
          "host reports pending reliable delivery failure when its recipient leaves");
    mn_get_info(host, &host_info);
    CHECK(host_info.player_count == 1, "host removes players who leave");

    printf("MultiNet tests passed (16-player room, snapshots, messages, match start, leave).\n");
cleanup:
    for (j = 0; j < extra_count; ++j) if (extra_clients[j]) mn_destroy(extra_clients[j]);
    if (rejected_client) mn_destroy(rejected_client);
    if (spoof_client) mn_destroy(spoof_client);
    if (bad_key_scanner) mn_destroy(bad_key_scanner);
    if (auto_scanner) mn_destroy(auto_scanner);
    if (scanner) mn_destroy(scanner);
    if (client) mn_destroy(client);
    if (host) mn_destroy(host);
    return result;
}
