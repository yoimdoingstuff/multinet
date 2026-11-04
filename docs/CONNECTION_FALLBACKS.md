# Connection fallbacks

MultiNet deliberately does not depend on one discovery method. Local networks differ, and broadcast packets are often filtered even when normal UDP unicast is allowed.

Use these connection paths in order. The default `mn_discover_auto()` / demo `scan` runs the first two local discovery methods together rather than making you choose one:

1. **Combined local discovery.** Call `mn_discover_auto()`: it sends global LAN broadcast, subnet broadcasts on every enumerated IPv4 adapter and mDNS/DNS-SD queries on each active non-loopback adapter. The host advertises each active IPv4 address in its mDNS response, and the client prefers the address matching its local subnet. This helps multi-homed PCs with Ethernet, Wi-Fi, Wi-Fi Direct and Bluetooth PAN active together.
2. **Broadcast-only discovery.** Call `mn_discover()`, continue calling `mn_update()`, and read `mn_get_rooms()`. This is the easiest option when the local subnet forwards UDP broadcast.
3. **Directed/unicast discovery.** When broadcast is disabled or no rooms appear, call `mn_discover_at(instance, known_host, port)`. MultiNet sends a normal discovery request directly to that IPv4 address or hostname and retries it for the discovery window. This requires knowing the host address but does not require subnet broadcast. When the PC has multiple active adapters and you need to force the route, use `mn_discover_at_from(instance, local_ipv4, known_host, port)` to bind the probe to Ethernet, Wi-Fi, Wi-Fi Direct or Bluetooth PAN.
4. **DNS-SD/mDNS-only discovery.** Call `mn_discover_mdns()` or run `multinet_demo scan-mdns`. It looks for `_multinet._udp.local` service advertisements. This is useful when broadcast discovery is filtered but link-local multicast is allowed. mDNS is optional; check `mn_info.mdns_available` because another service or the OS may already own UDP port 5353.
5. **Manual direct join.** When room discovery is blocked entirely, call `mn_join(instance, host, port, player_name)`. The host still needs to be reachable on its configured UDP port.
6. **Port fallback.** The host and client can choose a different UDP port in `mn_config.port` or the join argument. This helps with a local port conflict, but both sides must agree on the selected port and the firewall must permit it.

The Windows GUI demo is built as `multinet_demo.exe` and can be launched by double-clicking. Its **Ping** control measures latency to the host from a client, or to the selected remote player from the host. The terminal variant is `multinet_cli_demo.exe`; Linux continues to use `multinet_demo`.

There is no hidden relay or tunnel. These are ordinary UDP connection attempts over the network the user is already allowed to use. A firewall or network policy may block all of them; MultiNet reports a timeout/error instead of trying to bypass that restriction.

## Using Bluetooth PAN, Wi-Fi Direct, hotspots and Ethernet

MultiNet uses ordinary IP networking. It can use Ethernet, Wi-Fi, phone hotspots, USB tethering, Wi-Fi Direct and Bluetooth Personal Area Network (PAN) when the operating system has created an IP interface for that connection. Pair/connect the devices through the normal OS network settings first, then call `mn_get_interfaces()` to list their IPv4 addresses and adapter types. In Windows, the GUI demo lists active adapters in its Network adapter box; choose the Bluetooth PAN or Wi-Fi Direct entry before hosting, joining or searching. Use **Refresh adapters** after connecting a new OS-managed link so it appears without restarting the app. The CLI demo exposes the same choice through `interfaces`, `host-on`, `join-from` and `scan-on` commands. Automatic discovery sends broadcast and mDNS probes across active IPv4 adapters instead of relying only on the OS default route.

This is Bluetooth PAN networking over an OS-managed IP adapter. It is distinct from direct Bluetooth Low Energy advertising and RFCOMM. A separate experimental Windows-only RFCOMM stream API is now available through `mn_rfcomm_*`; the double-clickable `multinet_rfcomm_demo.exe` advertises and discovers a small one-peer service using Windows SDP and transfers text over a non-blocking stream. When secure mode is checked, Windows socket options require Bluetooth authentication and link encryption, with pairing/passkey UI handled by Windows. The RFCOMM test backend is not yet integrated into `mn_instance` or the 16-player game room, and the byte-stream API requires applications to frame messages themselves. It is a Windows desktop feature, not BLE GATT, and it does not promise Apple AirDrop/Quick Share or Android RFCOMM interoperability. MultiNet does not silently pair devices or toggle radios. Bluetooth PAN and Wi-Fi Direct availability depends on OS version, drivers, device support and administrator policy. If they do not create usable links, use ordinary Wi-Fi/Ethernet or a supported hotspot.

## Native Bluetooth RFCOMM (experimental)

RFCOMM is a Bluetooth Classic serial-style byte stream, not an IP adapter and not a UDP datagram socket. The optional `mn_rfcomm_*` API is deliberately separate from `mn_instance`: it currently supports one peer per endpoint and provides service advertising, nearby service discovery through Windows SDP, optional link authentication/encryption and non-blocking stream reads/writes. It does not yet carry MultiNet room packets or connect directly to Golfgame. The test GUI is `multinet_rfcomm_demo.exe` on Windows.

To test, run the GUI on two Bluetooth-capable Windows PCs. Advertise on one, scan and connect from the other, then exchange text in both directions. Windows may show its pairing/passkey prompt. A discovery scan can take several seconds because Windows performs a Bluetooth inquiry and queries SDP records on devices. RFCOMM loopback is not supported by Windows, so CI can compile and smoke-test the GUI but cannot replace a physical two-device test. The current API transfers bytes, not messages; callers must implement their own framing and handle partial reads/writes.

## Troubleshooting order

- Confirm both devices are on the same permitted LAN and that the host is actually hosting.
- Try automatic discovery.
- If the scan shows no room, try mDNS/DNS-SD, then a known host IPv4 address with directed discovery. Choose the local interface explicitly with `mn_discover_at_from()` when the default route is not the intended adapter.
- If directed discovery gets no offer, try direct join to the same host and port.
- If the device has multiple adapters, enumerate them with `mn_get_interfaces()` and bind both sides to the intended adapter with `mn_host_on()` / `mn_join_from()`.
- If the host cannot bind its port, choose another port on the host and use that exact port for scan/join.
- If none works, check ordinary OS firewall/router settings with the network administrator. Do not disable managed controls or run covert tunnels.
