# MultiNet roadmap

## Core extraction

- [x] Extract the reusable UDP LAN transport from Golfgame.
- [x] Replace the global singleton with independent C11 instances.
- [x] Add portable, explicit packet serialization and protocol/app identifiers.
- [x] Add player roster snapshots, room discovery, joining, leaving and host start.
- [x] Add an event queue and application-defined message channels.
- [x] Add 16-player loopback integration coverage.
- [x] Add a live interactive demo and Windows/Linux CI.

## Reliability and security

- [x] Add packet sequence numbers and stale/out-of-order snapshot rejection.
- [x] Add basic per-peer cooldown rate limits for state updates and application messages.
- [x] Add a reliable message API with acknowledgement, retry and delivery IDs.
- [x] Expose delivery IDs plus per-recipient acknowledgement/failure events to applications.
- [x] Exercise reliable broadcast fan-out and one acknowledgement from each of 15 recipients.
- [x] Re-ACK duplicate reliable packets during cooldown and only remember accepted messages after queue capacity is reserved.
- [x] Validate peer-controlled state against configurable position/velocity bounds and application callbacks.
- [x] Add a host-controlled IPv4 source allow-list for new nearby-device admissions.
- [ ] Add cryptographic device identity and challenge-response pairing; an IP allow-list is not authentication.
- [ ] Add optional encrypted and authenticated transport for untrusted networks.

## Transport and discovery

- [x] Add directed/unicast room discovery as a fallback when LAN broadcast is filtered.

- [x] Add DNS-SD/mDNS service discovery as another LAN room-discovery fallback.
- [x] Add one automatic discovery mode that runs global broadcast, subnet-directed broadcast and mDNS together.
- [ ] Add discovery tests across isolated local interfaces where CI permits.
- [x] Add basic client-to-host and host-to-client ping/latency measurement.
- [x] Enumerate OS IPv4 adapters and support explicit binding for Ethernet, Wi-Fi, Wi-Fi Direct and Bluetooth PAN IP interfaces.
- [ ] Add native Bluetooth RFCOMM support as an optional transport backend.
- [ ] Add IPv6 transport.
- [ ] Add TCP as an optional direct-connect transport for networks where UDP is unavailable.

## Integration and packaging

- [ ] Integrate this library into Golfgame and remove its duplicated `tg_network` implementation.
- [x] Add package-config generation and an installed-consumer CI test.
- [ ] Publish a versioned release archive.
- [ ] Add examples for a non-game chat/message app and a simple replicated 2D object.
- [x] Add an optional Clang/libFuzzer target for malformed headers, lengths, room offers and player-state payloads.
- [ ] Add Windows multi-process LAN manual test checklist.

## Explicit non-goals

- No covert tunnels, firewall-policy bypasses or unapproved proxies.
- No claim of secure public-internet matchmaking until authentication, encryption, reliability and authoritative game validation exist.
- No game-specific physics, ball simulation, graphics or UI in the core library.
