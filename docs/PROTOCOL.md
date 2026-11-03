# Wire protocol and security notes

## Packet framing

Every datagram begins with a 16-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `MNET` |
| 4 | 1 | Protocol version (`4`) |
| 5 | 1 | Packet type |
| 6 | 2 | Total datagram size, including header |
| 8 | 4 | Application ID |
| 12 | 4 | Room ID |

Integers are encoded in big-endian byte order. Float fields are 32-bit IEEE-754 values encoded using their bit representation in big-endian order. The maximum supported UDP datagram size is 1472 bytes. No compiler-specific structure packing or host-endian struct dumps are used.

Packet types on the game socket include discovery, offer, join, welcome, player state, snapshot, leave, match start, best-effort and reliable application messages, acknowledgements, ping/pong, pairing denial, the secure envelope, pairing challenge and pairing proof. DNS-SD/mDNS discovery uses standard DNS messages on a separate optional UDP 5353 socket. Version and app ID are checked before a packet is handled. Payload lengths are checked against the exact expected size before fields are read. Version 2 added wrap-safe sequence numbers to state/snapshot packets; version 3 added reliable messages and ping/latency; version 4 adds optional authenticated encryption and device pairing.

A player snapshot contains a count, match-started flag and up to 16 fixed-size player records. A player record contains ID, score, flags, packed RGBA colour, four appearance identifiers, a fixed-size name, position, quaternion rotation and velocity.

## Authenticated packet envelope

When a 32-byte room key is configured on an instance, all game-socket packets are wrapped in AES-256-GCM. The outer packet retains the normal 16-byte header with packet type `MN_PKT_SECURE`; its payload is a 12-byte nonce, a one-byte inner packet type, the encrypted inner payload and a 16-byte GCM tag. The authenticated additional data (AAD) is the complete outer header plus the inner packet type. The original inner header is reconstructed after successful decryption. An instance with encryption enabled rejects unauthenticated plaintext game packets.

The nonce is an eight-byte random prefix generated for each instance plus a four-byte monotonically increasing counter. The sender stops sending secure packets rather than reusing a nonce after the counter is exhausted. Receivers maintain a 64-packet replay window for each source address/nonce prefix. Authentication is checked before replay state is updated. Windows uses BCrypt/CNG; Unix-like builds use OpenSSL EVP. If built with `MULTINET_ENABLE_ENCRYPTION=OFF`, crypto APIs report unavailable.

mDNS/DNS-SD remains ordinary clear-text DNS on its separate UDP 5353 socket. It advertises room metadata and is not authenticated or confidential. Applications should treat it as discovery convenience only; the actual game-socket join still requires the room key.

## Session model

The host owns room membership and assigns player IDs. The optional IPv4 source allow-list is an additional coarse filter and does not replace identity checks. Each instance has a random 128-bit device ID and 256-bit private device secret. Applications can persist and restore both; the host stores a matching ID/secret pair for each paired device. When pairing-required mode is enabled, unknown IDs generate an approval event. After approval, the host sends a fresh 32-byte challenge. The client responds with HMAC-SHA256 over `challenge_nonce || device_id || room_id_be || app_id_be`. A valid proof is required before the host assigns a player ID and sends WELCOME. Paired devices repeat this challenge-response on later joins.

The client sends its private device secret inside the encrypted JOIN packet so the host can store it after the user approves the first pairing. Because the room key is shared, another room member with that key can decrypt and observe that initial secret if it captures the packet. This design authenticates paired devices against outsiders without the room key; it does not defend against malicious holders of the shared key. Use secure local storage for persisted secrets and use a separately provisioned per-device public-key protocol where that stronger threat model matters.

Each admitted client also receives a per-session 64-bit random token. Client state, leave and application-message packets must include that token and originate from the address bound to the session. Tokens are generated using the operating system random source (`BCryptGenRandom` on Windows and `/dev/urandom` on POSIX).

## Delivery and authority

Player states are periodic snapshots. They are not individually acknowledged. Custom messages are best-effort UDP datagrams and may be lost, duplicated or reordered. Player-state updates and host snapshots include wrap-safe sequence numbers, and the host applies a simple per-peer state/message cooldown. Best-effort application messages remain unacknowledged. The reliable channel adds message IDs, ACK packets, bounded retransmission and duplicate suppression. Duplicate reliable packets are re-ACKed even during the ordinary message cooldown, and received message IDs are only remembered after the host has confirmed queue capacity. A client-to-host reliable call confirms delivery to the host; the host then independently queues delivery to the other room members. Pending deliveries are held in a finite in-memory queue and are not persisted across disconnects or process restarts. Applications can obtain the delivery ID from the extended send API and receive per-recipient acknowledged/failed events. Duplicate packets are re-ACKed even during the message cooldown; the receiver only records an ID as seen after it has capacity to accept the delivery. Ping packets measure one outstanding round trip at a time and expire after five seconds.

The host relays client-reported player state, but can now enforce configurable absolute position and velocity bounds and invoke an application callback before committing each accepted update. The default bounds preserve the broad 1,000,000-unit finite-value ceiling. Games should use the callback to validate score changes, flags, shot state, hole completion and other gameplay-specific rules. A rejected state does not refresh the peer timeout or replace the last accepted state. These checks reduce accidental or obviously invalid state injection; they do not make the simulation authoritative or prevent a modified client from choosing values that happen to pass the configured rules. Cosmetic identifiers should never confer physics or visibility advantages.

## Network scope

Discovery supports IPv4 global/subnet broadcast, explicit-interface directed broadcast, directed unicast probes and DNS-SD/mDNS. Manual IPv4/hostname joining is supported. There is no NAT traversal, public matchmaking, IPv6 transport, raw BLE discovery, proxy, tunnel or multi-hop relay. Use only networks on which ordinary game traffic is permitted.

## Planned protocol hardening

- [x] Sequence numbers and replay/out-of-order handling.
- [x] Per-peer packet/message rate limits and configurable absolute state bounds.
- [x] Host-side application callback for game-specific remote-state validation.
- [ ] Host-side authoritative input/shot validation and match-result verification.
- [x] Optional AES-256-GCM authenticated packet transport with replay checks (shared pre-shared room-key model).
- [x] Device ID/secret pairing with host approval and HMAC-SHA256 challenge-response.
- IPv6 and standards-based service discovery.
- [x] Add a Clang/libFuzzer harness for packet framing, room offers and player-state payload decoding.
- Explicit player identity/pairing and encrypted peer authentication for untrusted networks.
