# Wire protocol and security notes

## Packet framing

Every datagram begins with a 16-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `MNET` |
| 4 | 1 | Protocol version (`3`) |
| 5 | 1 | Packet type |
| 6 | 2 | Total datagram size, including header |
| 8 | 4 | Application ID |
| 12 | 4 | Room ID |

Integers are encoded in big-endian byte order. Float fields are 32-bit IEEE-754 values encoded using their bit representation in big-endian order. The maximum supported UDP datagram size is 1472 bytes. No compiler-specific structure packing or host-endian struct dumps are used.

Packet types on the game socket are discovery, offer, join, welcome, player state, snapshot, leave, match start, best-effort application message, reliable application message, reliable acknowledgement, ping request and ping response. DNS-SD/mDNS discovery uses standard DNS messages on a separate optional UDP 5353 socket. Version and app ID are checked before a packet is handled. Payload lengths are checked against the exact expected size before fields are read. Version 2 added a 32-bit sequence number to client player-state packets and host snapshots; receivers ignore duplicated, stale or reordered state/snapshot packets. Version 3 adds reliable application messages and ping/latency packets.

A player snapshot contains a count, match-started flag and up to 16 fixed-size player records. A player record contains ID, score, flags, packed RGBA colour, four appearance identifiers, a fixed-size name, position, quaternion rotation and velocity.

## Session model

The host owns room membership and assigns player IDs. A joining client receives a per-session 64-bit random token. Client state, leave and application-message packets must include that token and originate from the address bound to the session. Tokens are generated using the operating system random source (`BCryptGenRandom` on Windows and `/dev/urandom` on POSIX).

The token is a basic session credential, not encryption. Traffic is not confidential, packets are not cryptographically signed, and the host is not a complete anti-cheat authority. A hostile local-network peer may still spoof, replay or flood traffic in ways that require stronger protocol protections before this is used outside trusted LAN play.

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
- Optional encrypted and authenticated transport for untrusted networks.
- IPv6 and standards-based service discovery.
- [x] Add a Clang/libFuzzer harness for packet framing, room offers and player-state payload decoding.
- Explicit player identity/pairing and encrypted peer authentication for untrusted networks.
