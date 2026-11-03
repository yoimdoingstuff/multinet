# API guide

The public interface is a C API in `include/multinet.h`. Include that header and link the `multinet` static library. Functions take an explicit `mn_instance *`; there is no process-global active room. After installation, consumers can use `find_package(MultiNet CONFIG REQUIRED)` and `target_link_libraries(your_app PRIVATE MultiNet::MultiNet)`.

## Instance setup

Call `mn_config_init()` to populate defaults, optionally change the UDP port, application ID, player cap or timing values, then call `mn_create()`. Destroy the instance with `mn_destroy()`. `mn_get_interfaces()` enumerates OS-managed IPv4 adapters, including loopback, Ethernet, Wi-Fi, Wi-Fi Direct and adapters identified as Bluetooth PAN.

The default app ID is `MN_DEFAULT_APP_ID`. Two games using different IDs will not see each other's discovery offers or accept each other's packets, even when using the same UDP port. Give each application a stable, unique ID for release builds.

`mn_create()` opens an ephemeral local UDP socket. Hosting switches that socket to the configured listening port; joining keeps the ephemeral source port and targets the supplied server port. A second independent `mn_instance` may be created in the same process and used as a separate test client.

## Hosting and joining

- `mn_host(instance, player_name, room_name, max_players)` starts a room on all local IPv4 interfaces. The host is player ID 1. `max_players` includes the host and is clamped to 2–16.
- `mn_host_on(..., local_ipv4)` binds the host to one selected local adapter address.
- `mn_discover_on(instance, interface)` sends discovery using that adapter's directed IPv4 broadcast address.
- `mn_join_from(instance, local_ipv4, host, port, player_name)` binds the client to a chosen local adapter before connecting. This can select an Ethernet, Wi-Fi, Wi-Fi Direct or Bluetooth PAN IP link.
- `mn_discover()` starts a broadcast-only scan. `mn_discover_auto()` is the recommended default: it sends global broadcast, directed subnet-broadcast probes on each usable OS IPv4 interface, and mDNS/DNS-SD concurrently, then retries them during the discovery window. Call `mn_update()` while it runs, then query `mn_get_rooms()`.
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

For an application-defined event stream, call `mn_send_message()`. Channel IDs are chosen by the game. A target ID of zero broadcasts to the room; host code may also target one player ID. Clients send to the host, which relays the message to matching recipients. Messages are best-effort and limited to `MN_MAX_MESSAGE_BYTES`.

Use `mn_send_reliable_message()` for lobby commands, ready state, match results and other events that must arrive. It queues each delivery, assigns a message ID, waits for an acknowledgement, retransmits on timeout and suppresses duplicate deliveries. Clients send to the host (target 0); the host may target a player or broadcast. The bounded queue holds up to `MN_MAX_RELIABLE_PENDING` pending recipient deliveries. If the queue is full, the call fails and the application should retry later. After the retry limit, `MN_EVENT_ERROR` reports the failed delivery. A client-to-host ACK confirms delivery to the host; the host independently queues fan-out to the other room members. This is reliable delivery while the peer remains reachable, not durable storage across disconnects or process restarts.

Drain `mn_poll_event()` each frame or on a regular tick. Events are FIFO and include connection changes, player joins/leaves, room discovery, match start, application messages and errors. The queue has a finite capacity; drain it regularly to avoid dropping older events under heavy traffic.

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

The current implementation is intended to be called from one application thread. Do not call `mn_update()`, `mn_leave()` or state-mutating functions concurrently on the same instance. Multiple independent instances are supported, but each instance should have one owning thread.

## Error handling

Creation failure returns `NULL`. Most lifecycle methods return non-zero on success. `mn_get_info()` copies the most recent human-readable error to `last_error`; `MN_EVENT_ERROR` is also queued. Network send functions use UDP and cannot promise remote delivery merely because the local send call was accepted.
