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
- [ ] Add a reliable message API with acknowledgement, retry and delivery IDs.
- [ ] Validate peer-controlled state against configurable bounds and application callbacks.
- [ ] Add an explicit allow-list / pairing flow for nearby devices.
- [ ] Add optional encrypted and authenticated transport for untrusted networks.

## Transport and discovery

- [x] Add directed/unicast room discovery as a fallback when LAN broadcast is filtered.

- [ ] Add IPv6 transport.
- [ ] Add standards-based mDNS/DNS-SD discovery.
- [ ] Add discovery tests across isolated local interfaces where CI permits.
- [ ] Add a connection-quality / ping measurement API.
- [ ] Investigate documented Wi-Fi Direct or Bluetooth PAN support as optional transports; keep UDP/Wi-Fi LAN as the default.

## Integration and packaging

- [ ] Integrate this library into Golfgame and remove its duplicated `tg_network` implementation.
- [x] Add package-config generation and an installed-consumer CI test.
- [ ] Publish a versioned release archive.
- [ ] Add examples for a non-game chat/message app and a simple replicated 2D object.
- [ ] Add fuzz tests for malformed headers, lengths and player-state payloads.
- [ ] Add Windows multi-process LAN manual test checklist.

## Explicit non-goals

- No covert tunnels, firewall-policy bypasses or unapproved proxies.
- No claim of secure public-internet matchmaking until authentication, encryption, reliability and authoritative game validation exist.
- No game-specific physics, ball simulation, graphics or UI in the core library.
