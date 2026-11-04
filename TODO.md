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
- [x] Add persistent cryptographic device identity and host-approved HMAC-SHA256 challenge-response pairing.
- [x] Add optional AES-256-GCM authenticated packet transport using a shared pre-shared room key; document its group-key limitations.

## Transport and discovery

- [x] Add directed/unicast room discovery as a fallback when LAN broadcast is filtered.
- [x] Add interface-bound directed/unicast discovery for Bluetooth PAN, Wi-Fi Direct and multi-adapter PCs.

- [x] Add DNS-SD/mDNS service discovery as another LAN room-discovery fallback.
- [x] Add one automatic discovery mode that runs global broadcast, subnet-directed broadcast and mDNS together.
- [x] Send and receive mDNS queries/announcements on all active IPv4 adapters; advertise multiple host A records and select the client-local subnet address.
- [ ] Add discovery tests across physically isolated Ethernet/Wi-Fi/Bluetooth PAN interfaces where CI permits; loopback CI cannot prove hardware routing.
- [x] Add basic client-to-host and host-to-client ping/latency measurement.
- [x] Enumerate OS IPv4 adapters and support explicit binding for Ethernet, Wi-Fi, Wi-Fi Direct and Bluetooth PAN IP interfaces.
- [x] Add an experimental Windows RFCOMM one-peer stream API with SDP service registration/discovery, optional OS authentication + link encryption and a double-clickable test GUI.
- [x] Add a transport-neutral framed-message API with application channels, fixed-endian message headers, bounded queues and correct partial read/write handling.
- [x] Add an RFCOMM-to-stream adapter and update the Bluetooth GUI to use the framed-message layer.
- [x] Add stream reset/reconnect lifecycle support that drops stale partial frames and queued data; the Windows RFCOMM host demo can accept a replacement peer after disconnect.
- [ ] Integrate stream transports into full MultiNet room/session semantics with multiple peers, identity, host approval, discovery and reconnection; the new framed API is currently a one-peer message channel, not a replacement for `mn_instance` or Golfgame.
- [ ] Test RFCOMM discovery, pairing, secure connection, disconnect and reconnect across two physical Windows PCs.
- [ ] Add IPv6 transport.
- [x] Add direct IPv4 TCP host/client streams with non-blocking connect, one-peer host listeners, replacement-client acceptance, framed-message adaptation and loopback coverage; the backend is not yet part of `mn_instance` room semantics.
- [x] Add a double-clickable Windows TCP test GUI using framed application messages and a manual two-PC test workflow.
- [x] Add a native Windows GUI demo with adapter selection, host/search/join/leave controls, live player list, messages, ping and status log; refresh adapter enumeration after Bluetooth PAN/Wi-Fi Direct links are connected.

## Integration and packaging

- [x] Integrate MultiNet into Golfgame through a thin `tg_net_*` compatibility adapter; remove the duplicated UDP transport implementation.
- [x] Add package-config generation and an installed-consumer CI test.
- [ ] Publish a versioned release archive (tag-triggered Linux/Windows packaging and GitHub Release workflow added; publishing requires pushing a matching version tag).
- [x] Add a reliable chat-message example using application-defined channels and delivery acknowledgements.
- [x] Add a simple replicated 2D object example using player position state.
- [x] Add an optional Clang/libFuzzer target for malformed headers, lengths, room offers and player-state payloads.
- [x] Document the Windows multi-process LAN manual test checklist (physical-device execution remains a release validation task).

## Explicit non-goals

- No covert tunnels, firewall-policy bypasses or unapproved proxies.
- No claim of secure public-internet matchmaking until authentication, encryption, reliability and authoritative game validation exist.
- No game-specific physics, ball simulation, graphics or UI in the core library.
