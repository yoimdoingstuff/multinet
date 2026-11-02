# Wire protocol and security notes

## Packet framing

Every datagram begins with a 16-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `MNET` |
| 4 | 1 | Protocol version (`1`) |
| 5 | 1 | Packet type |
| 6 | 2 | Total datagram size, including header |
| 8 | 4 | Application ID |
| 12 | 4 | Room ID |

Integers are encoded in big-endian byte order. Float fields are 32-bit IEEE-754 values encoded using their bit representation in big-endian order. The maximum supported UDP datagram size is 1472 bytes. No compiler-specific structure packing or host-endian struct dumps are used.

Packet types are discovery, offer, join, welcome, player state, snapshot, leave, match start and application message. Version and app ID are checked before a packet is handled. Payload lengths are checked against the exact expected size before fields are read.

A player snapshot contains a count, match-started flag and up to 16 fixed-size player records. A player record contains ID, score, flags, packed RGBA colour, four appearance identifiers, a fixed-size name, position, quaternion rotation and velocity.

## Session model

The host owns room membership and assigns player IDs. A joining client receives a per-session 64-bit random token. Client state, leave and application-message packets must include that token and originate from the address bound to the session. Tokens are generated using the operating system random source (`BCryptGenRandom` on Windows and `/dev/urandom` on POSIX).

The token is a basic session credential, not encryption. Traffic is not confidential, packets are not cryptographically signed, and the host is not a complete anti-cheat authority. A hostile local-network peer may still spoof, replay or flood traffic in ways that require stronger protocol protections before this is used outside trusted LAN play.

## Delivery and authority

Player states are periodic snapshots. They are not individually acknowledged. Custom messages are best-effort UDP datagrams and may be lost, duplicated or reordered. The library currently has no application-level sequence numbers, retransmission queue or rate limiter. Add those features before relying on messages for persistent inventory, purchases, match results or other state that must not be lost.

The host relays client-reported player state. Game code must treat it as untrusted and independently validate speed, position, score changes, hole completion and any gameplay-affecting action. Cosmetic identifiers should never confer physics or visibility advantages.

## Network scope

Discovery uses IPv4 subnet broadcast on the configured UDP port. Manual IPv4/hostname join is supported. There is no NAT traversal, public matchmaking, IPv6 transport, mDNS/DNS-SD, BLE/Wi-Fi Direct discovery, proxy, tunnel or multi-hop relay. Use only networks on which ordinary game traffic is permitted.

## Planned protocol hardening

- Sequence numbers and replay/out-of-order handling.
- Per-peer packet/message rate limits and maximum state-change thresholds.
- Host-side authoritative input/shot validation and match-result verification.
- Optional reliable message channel with acknowledgement/retry and delivery IDs.
- IPv6 and standards-based service discovery.
- Explicit player identity/pairing and encrypted peer authentication for untrusted networks.
