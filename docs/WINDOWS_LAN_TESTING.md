# Windows multi-process LAN test checklist

These tests are for two or more physical Windows PCs on a network where ordinary local multiplayer traffic is permitted. Loopback CI cannot verify router behaviour, Windows Firewall prompts, adapter selection, or real Wi-Fi/Ethernet routing.

## Before testing

- [ ] Use two Windows PCs on the same permitted LAN. Record Windows version/build and the network adapter type (Ethernet, Wi-Fi, Wi-Fi Direct or Bluetooth PAN).
- [ ] Make sure both builds use the same MultiNet version, application ID and UDP port.
- [ ] Check that each PC has a valid IPv4 address on the intended adapter. Use `ipconfig` and the demo's `interfaces` command.
- [ ] Start with the default UDP port `27890`. If the host firewall prompts, allow the demo only on the intended Private network profile. Do not disable Windows Firewall.
- [ ] Record the host's local IPv4 address and the selected client-side interface address.

## Basic join and replication

- [ ] Start the host on PC A using `multinet_demo host HostName "LAN Test"`.
- [ ] From PC B, discover using `multinet_demo scan` and confirm the room appears.
- [ ] Join the discovered room and confirm both PCs report two players.
- [ ] Move the demo player on PC B. Confirm PC A receives the updated position.
- [ ] Send a message in both directions and confirm the expected recipient receives it.
- [ ] Start a match on PC A and confirm PC B receives the match-started state.
- [ ] Leave from PC B and confirm PC A receives the player-left event.

## Discovery and connection fallback checks

Run each case separately and record whether it succeeds:

- [ ] Automatic discovery with broadcast and mDNS available.
- [ ] Broadcast disabled on the client; direct unicast discovery to the host address and port.
- [ ] Direct unicast discovery bound to the chosen local adapter using `mn_discover_at_from()`.
- [ ] Manual direct join by host IPv4 address without performing discovery.
- [ ] Host and client bound to explicit Ethernet addresses.
- [ ] Host and client bound to explicit Wi-Fi addresses.
- [ ] If available and permitted, test OS-managed Wi-Fi Direct and Bluetooth PAN IP adapters.
- [ ] With multiple adapters connected, confirm the GUI adapter dropdown and `join-from` select the intended source adapter.
- [ ] With Ethernet and Wi-Fi active together, verify mDNS returns the address on the client's matching subnet.
- [ ] If available and permitted, repeat discovery/join across Bluetooth PAN and Wi-Fi Direct using the GUI adapter selector.
- [ ] Repeat with a non-default UDP port and verify both endpoints use the same configured port.

Do not simulate a blocked network by disabling security controls on a managed device. Use a test network or an administrator-approved firewall rule.

## Admission and state validation

- [ ] Enable the host IPv4 allow-list and add the client IPv4 address. Confirm the client joins.
- [ ] Remove the client address while it is connected. Confirm the existing session remains active.
- [ ] Leave and attempt to rejoin from an address not on the list. Confirm the host does not admit it.
- [ ] Re-add the address and confirm the client's retry loop joins without restarting either process.
- [ ] Exercise strict position/velocity bounds and the application state validator. Confirm accepted state replicates and rejected state does not replace the last accepted state.

Remember: the IPv4 allow-list is only a source-address filter. It is not cryptographic authentication or secure device pairing.

## Reliability and room capacity

- [ ] Send reliable messages in both directions and confirm the acknowledgement event is reported.
- [ ] Send a host reliable broadcast and confirm each connected client receives it.
- [ ] Disconnect a recipient while a reliable delivery is pending. Confirm the host reports delivery failure.
- [ ] Join enough separate client processes to reach the 16-player room limit.
- [ ] Confirm the next client is not admitted while the room is full, then confirm it can join after a slot opens.

## Results to record

For each run, record date, build commit, Windows version, adapter type, host/client addresses (keep private when sharing logs), UDP port, discovery method, join result, message result, ping result, and any relevant `mn_info.last_error` text.

A successful checklist run is a manual compatibility result for the tested network only. It is not proof that all routers, guest Wi-Fi networks, VPNs, managed school networks, or public internet connections will permit the traffic.
