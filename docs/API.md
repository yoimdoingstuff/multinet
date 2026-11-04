# API guide

The public interface is a C API in `include/multinet.h`. Include that header and link the `multinet` static library. Functions take an explicit `mn_instance *`; there is no process-global active room. After installation, consumers can use `find_package(MultiNet CONFIG REQUIRED)` and `target_link_libraries(your_app PRIVATE MultiNet::MultiNet)`.

## Instance setup

Call `mn_config_init()` to populate defaults, optionally change the UDP port, application ID, player cap or timing values, then call `mn_create()`. Destroy the instance with `mn_destroy()`. `mn_get_interfaces()` enumerates OS-managed IPv4 adapters, including loopback, Ethernet, Wi-Fi, Wi-Fi Direct and adapters identified as Bluetooth PAN.

The default app ID is `MN_DEFAULT_APP_ID`. Two games using different IDs will not see each other's discovery offers or accept each other's packets, even when using the same UDP port. Give each application a stable, unique ID for release builds.

The source-address allow-list is only a coarse local-network admission filter. IPv4 addresses can change under DHCP and may be spoofed on some networks. It is not device identity or cryptographic pairing. Use it as an optional first admission gate, independently of the device pairing feature below.

`mn_create()` opens an ephemeral local UDP socket. Hosting switches that socket to the configured listening port; joining keeps the ephemeral source port and targets the supplied server port. A second independent `mn_instance` may be created in the same process and used as a separate test client.

## Authenticated encryption and device pairing

### Configure a room key

Encryption is optional at build time and enabled by default. Windows uses the built-in BCrypt/CNG provider; Unix-like builds use OpenSSL Crypto. Call `mn_crypto_available()` to check availability. Generate a random 32-byte key on one trusted instance with `mn_generate_room_key(key)`, then securely distribute the same key to every authorized host/client. This is a pre-shared key, not a key exchange.

Call `mn_set_room_key(instance, key)` while the instance is offline and before discovery, hosting or joining. All game UDP packets are then wrapped in AES-256-GCM authenticated encryption, and plaintext game packets are rejected. Clear it with `mn_clear_room_key()` while offline; this also disables pairing-required mode and clears the stored paired-device credentials. When encryption is disabled in CMake, `mn_crypto_available()` returns zero and room-key/pairing setup fails cleanly.

The mDNS/DNS-SD discovery socket remains separate and uses clear-text DNS messages. Room names, host names and player counts advertised through mDNS are therefore not confidential. The actual game-socket join and subsequent gameplay traffic still require the configured room key.

### Persistent device identity

Every instance starts with a random 128-bit device ID and a random 256-bit private device secret. Persist both in the application's secure settings store:
- `mn_get_device_id()` copies the public device ID.
- `mn_get_device_secret()` copies the private secret. Treat it like a password and protect it at rest.
- `mn_set_device_identity(instance, id, secret)` restores the pair on a newly created, offline instance.

Do not persist only the ID. The host verifies possession of the matching secret during pairing; a copied public ID without the secret is rejected.

### Host approval and paired-device trust

Pairing requires a configured room key. On the host, call `mn_set_pairing_required(host, 1)` before admitting clients. An unknown device receives an `MN_EVENT_PAIRING_REQUEST`; the event's `device_id` identifies the device. Show this request to the user, then call `mn_approve_pairing(host, event.device_id, 1)` to approve or pass zero to reject.

Approval does not immediately admit the device. The host stores its ID/secret, sends a fresh random challenge, and waits for the client to return an HMAC-SHA256 proof bound to the challenge, device ID, room ID and application ID. Only a valid proof receives a player ID and welcome packet. `MN_EVENT_PAIRING_APPROVED` is emitted after proof verification; rejection is reported with `MN_EVENT_PAIRING_REJECTED`.

The host can persist its trust store with `mn_get_paired_device_secret(host, id, secret)` and restore entries with `mn_pair_device(host, id, secret)` on a later instance. Use `mn_unpair_device()` to revoke a credential for future admissions or `mn_clear_paired_devices()` to remove all stored credentials. Unpairing does not forcibly disconnect a peer that is already in the room; that session remains active until it leaves or times out. Paired devices are challenged again on subsequent joins even when `pairing_required` is later turned off. Unknown devices can join without approval only when pairing-required mode is disabled.

### Security boundary

AES-GCM uses a 64-bit random per-instance nonce prefix plus a monotonically increasing 32-bit counter, with a replay window on receive. The room key is shared by all members: every member holding it can decrypt room traffic. The client's private device secret is first delivered to the host inside the group-key-encrypted JOIN packet, so another authorized room member capable of capturing traffic can observe that initial secret. HMAC challenge-response prevents a network outsider who lacks the room key from claiming a paired ID, but does not protect against a malicious participant who already has the shared room key. There is no automated key exchange, forward secrecy or secure public-internet matchmaking. Use an independently provisioned per-device public-key scheme for hostile-member threat models.

## Hosting and joining

- `mn_host(instance, player_name, room_name, max_players)` starts a room on all local IPv4 interfaces. The host is player ID 1. `max_players` includes the host and is clamped to 2–16.
- `mn_host_on(..., local_ipv4)` binds the host to one selected local adapter address.
- `mn_set_join_allowlist_enabled(instance, 1)` makes the host accept new room members only from addresses added with `mn_allow_join_ipv4()`. Use `mn_disallow_join_ipv4()` to revoke an address and `mn_clear_join_allowlist()` to remove all entries. The list holds up to `MN_MAX_JOIN_ALLOWLIST` IPv4 addresses, is disabled by default, and remains configured across `mn_leave()` calls on that instance. Changes take effect immediately for new admissions; already connected peers are not disconnected.
- `mn_discover_on(instance, interface)` sends discovery using that adapter's directed IPv4 broadcast address.
- `mn_join_from(instance, local_ipv4, host, port, player_name)` binds the client to a chosen local adapter before connecting. This can select an Ethernet, Wi-Fi, Wi-Fi Direct or Bluetooth PAN IP link.
- `mn_discover()` starts a broadcast-only scan. `mn_discover_auto()` is the recommended default: it sends global broadcast, directed subnet-broadcast probes on each usable OS IPv4 interface, and mDNS/DNS-SD queries on each active non-loopback adapter concurrently, then retries them during the discovery window. Hosts advertise all active IPv4 addresses in mDNS responses; clients prefer the address on their local subnet. Call `mn_update()` while it runs, then query `mn_get_rooms()`.
- `mn_discover_at(instance, address, port)` sends a unicast discovery probe to one known host and retries it during the discovery window. Use it when broadcast is disabled, blocked by a router, or suppressed by the local network.
- `mn_discover_mdns()` uses DNS-SD/mDNS on UDP 5353 to discover `_multinet._udp.local` services. Check `mn_info.mdns_available` first; the mDNS socket is optional and does not prevent the other discovery methods from working.
- `mn_join(instance, host, port, player_name)` joins by IPv4 address or hostname. A port of zero uses the configured port.
- `mn_join_room()` is a convenience wrapper around a discovered `mn_room`.
- `mn_start_match()` is host-only. Clients receive `MN_EVENT_MATCH_STARTED` and the match-started field in `mn_info`.
- `mn_leave()` leaves the current room or stops discovery. `mn_destroy()` releases the socket and the instance.

The API is asynchronous after `mn_join()` returns: the caller must continue calling `mn_update()` and watch `mn_get_info()` or the event queue until connected or timed out. Once connected, call `mn_ping(instance, 0)` on a client to measure its round trip to the host, or `mn_ping(instance, player_id)` on the host to measure one connected client. `mn_info.ping_ms`, `ping_available` and `ping_pending` expose the latest result and request state; requests time out after five seconds.

## Main loop

Call `mn_update(instance, dt_seconds)` regularly, ideally once per game frame. Socket operations are non-blocking. A large frame delta is clamped to 250 ms so a pause does not instantly expire every peer.

Call `mn_set_local_player()` with the latest local state. The host relays client state to the room in periodic snapshots. Use `mn_get_info()` for session details, `mn_get_players()` to copy the current roster, and `mn_get_player()` for ID-based lookup.

### Host-side remote-state validation

By default, incoming position and velocity components retain the broad finite-value safety ceiling of 1,000,000. A host can tighten those per-axis bounds with `mn_player_state_limits` and `mn_set_player_state_validation()`. Set either limit to zero to disable that additional bound. Invalid or negative limit values are rejected by the setter without changing the current policy.

The optional `mn_player_state_validator` callback runs only on the host, after decoding and built-in bounds checks but before the incoming state is committed. It receives the last accepted state and a candidate state whose player ID and name have already been replaced with host-assigned identity. Return non-zero to accept or zero to reject. This is where the game should validate application-specific fields such as score changes, flags, movement rules, or whether a player may claim to be holed out. Rejected state does not replace the last accepted state or refresh that peer's timeout. The packet sequence and cooldown are still consumed to prevent invalid-state spam from bypassing rate limiting.

Call the setter before hosting, or change the policy later from the instance's owning thread. The callback runs synchronously inside `mn_update()`; keep it fast and do not call state-mutating MultiNet functions on the same instance from inside it. The callback's `user_data` pointer remains owned by the application and must stay valid while registered. This is an application validation hook, not a substitute for cryptographic authentication or a fully authoritative game simulation.

For an application-defined event stream, call `mn_send_message()`. Channel IDs are chosen by the game. A target ID of zero broadcasts to the room; host code may also target one player ID. Clients send to the host, which relays the message to matching recipients. Messages are best-effort and limited to `MN_MAX_MESSAGE_BYTES`.

Use `mn_send_reliable_message()` for lobby commands, ready state, match results and other events that must arrive. It queues each delivery, assigns a message ID, waits for an acknowledgement, retransmits on timeout and suppresses duplicate deliveries. Use `mn_send_reliable_message_ex()` to receive the delivery ID, then match it against `MN_EVENT_MESSAGE_ACKNOWLEDGED` or `MN_EVENT_MESSAGE_FAILED`. Clients send to the host (target 0); the host may target a player or broadcast. The bounded queue holds up to `MN_MAX_RELIABLE_PENDING` pending recipient deliveries. A host broadcast produces one acknowledgement event per recipient, with `event.player_id` identifying that player; client-to-host acknowledgement events use player ID zero to identify the host. `event.delivery_id` carries the ID returned by the extended API. The event's `channel` identifies the application channel. If the queue is full, the call fails and the application should retry later. After the retry limit, `MN_EVENT_ERROR` reports the failed delivery. A client-to-host ACK confirms delivery to the host; the host independently queues fan-out to the other room members. This is reliable delivery while the peer remains reachable, not durable storage across disconnects or process restarts.

Drain `mn_poll_event()` each frame or on a regular tick. Events are FIFO and include connection changes, player joins/leaves, room discovery, match start, application messages and errors. The queue has a finite capacity; drain it regularly to avoid dropping older events under heavy traffic.

## Standalone examples

The `multinet_chat` target demonstrates a small message-oriented application. It sends a reliable text message on a caller-chosen channel and prints incoming message and acknowledgement/failure events.

The `multinet_2d_object` target demonstrates a renderer-independent replicated object. It stores the object's 2D coordinates in `mn_player.position.x` and `mn_player.position.y`, publishes local state with `mn_set_local_player()`, and reads remote states with `mn_get_players()`. Both examples are built and installed when `MULTINET_BUILD_DEMO` is enabled.

## Player data mapping for Golfgame

| MultiNet field | Suggested Golfgame use |
|---|---|
| `id`, `name` | Network player identity and scoreboard label |
| `position`, `rotation`, `velocity` | Remote ball transform and movement smoothing |
| `score` | Current-hole strokes or match score |
| `colour_rgba` | Ball material tint |
| `appearance[0]` | Hat/accessory ID |
| `appearance[1]` | Trail ID |
| `flags` | Moving, holed-out, in-game, or other game-defined state bits |

MultiNet does not render or simulate balls. The game owns physics, interpolation, camera, score validation and cosmetics.

## Threading

The current implementation is intended to be called from one application thread. Do not call `mn_update()`, `mn_leave()` or state-mutating functions concurrently on the same instance. Multiple independent instances are supported, but each instance should have one owning thread. A registered state-validation callback is invoked by `mn_update()` on that same owning thread.

## Physical Windows LAN testing

Use [`docs/WINDOWS_LAN_TESTING.md`](WINDOWS_LAN_TESTING.md) for a step-by-step two-PC test plan covering discovery fallbacks, explicit adapters, admission filtering, reliable messages, ping and the 16-player limit. Automated loopback tests cannot replace these physical-network checks.

## Error handling

Creation failure returns `NULL`. Most lifecycle methods return non-zero on success. `mn_get_info()` copies the most recent human-readable error to `last_error`; `MN_EVENT_ERROR` is also queued. Network send functions use UDP and cannot promise remote delivery merely because the local send call was accepted.
