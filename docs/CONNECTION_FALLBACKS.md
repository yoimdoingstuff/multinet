# Connection fallbacks

MultiNet deliberately does not depend on one discovery method. Local networks differ, and broadcast packets are often filtered even when normal UDP unicast is allowed.

Use these connection paths in order. The default `mn_discover_auto()` / demo `scan` runs the first two local discovery methods together rather than making you choose one:

1. **Combined local discovery.** Call `mn_discover_auto()`: it sends global LAN broadcast, subnet broadcasts on every enumerated IPv4 adapter and mDNS/DNS-SD queries on each active non-loopback adapter. The host advertises each active IPv4 address in its mDNS response, and the client prefers the address matching its local subnet. This helps multi-homed PCs with Ethernet, Wi-Fi, Wi-Fi Direct and Bluetooth PAN active together.
2. **Broadcast-only discovery.** Call `mn_discover()`, continue calling `mn_update()`, and read `mn_get_rooms()`. This is the easiest option when the local subnet forwards UDP broadcast.
3. **Directed/unicast discovery.** When broadcast is disabled or no rooms appear, call `mn_discover_at(instance, known_host, port)`. MultiNet sends a normal discovery request directly to that IPv4 address or hostname and retries it for the discovery window. This requires knowing the host address but does not require subnet broadcast.
4. **DNS-SD/mDNS-only discovery.** Call `mn_discover_mdns()` or run `multinet_demo scan-mdns`. It looks for `_multinet._udp.local` service advertisements. This is useful when broadcast discovery is filtered but link-local multicast is allowed. mDNS is optional; check `mn_info.mdns_available` because another service or the OS may already own UDP port 5353.
5. **Manual direct join.** When room discovery is blocked entirely, call `mn_join(instance, host, port, player_name)`. The host still needs to be reachable on its configured UDP port.
6. **Port fallback.** The host and client can choose a different UDP port in `mn_config.port` or the join argument. This helps with a local port conflict, but both sides must agree on the selected port and the firewall must permit it.

The Windows GUI demo is built as `multinet_demo.exe` and can be launched by double-clicking. The terminal variant is `multinet_cli_demo.exe`; Linux continues to use `multinet_demo`.

There is no hidden relay or tunnel. These are ordinary UDP connection attempts over the network the user is already allowed to use. A firewall or network policy may block all of them; MultiNet reports a timeout/error instead of trying to bypass that restriction.

## Using Bluetooth PAN, Wi-Fi Direct, hotspots and Ethernet

MultiNet uses ordinary IP networking. It can use Ethernet, Wi-Fi, phone hotspots, USB tethering, Wi-Fi Direct and Bluetooth Personal Area Network (PAN) when the operating system has created an IP interface for that connection. Pair/connect the devices through the normal OS network settings first, then call `mn_get_interfaces()` to list their IPv4 addresses and adapter types. In Windows, the GUI demo lists active adapters in its Network adapter box; choose the Bluetooth PAN or Wi-Fi Direct entry before hosting, joining or searching. The CLI demo exposes the same choice through `interfaces`, `host-on`, `join-from` and `scan-on` commands. Automatic discovery sends broadcast and mDNS probes across active IPv4 adapters instead of relying only on the OS default route.

This is Bluetooth PAN networking, not direct Bluetooth Low Energy advertising or RFCOMM. Native RFCOMM/BLE transport is still on the roadmap; it requires a separate stream/connection backend because the current transport is IPv4 UDP. MultiNet does not silently pair devices or toggle radios. Bluetooth PAN and Wi-Fi Direct availability depends on OS version, drivers, device support and administrator policy. If they do not create usable IPv4 addresses, use ordinary Wi-Fi/Ethernet or a supported hotspot.

## Troubleshooting order

- Confirm both devices are on the same permitted LAN and that the host is actually hosting.
- Try automatic discovery.
- If the scan shows no room, try mDNS/DNS-SD, then a known host IPv4 address with directed discovery.
- If directed discovery gets no offer, try direct join to the same host and port.
- If the device has multiple adapters, enumerate them with `mn_get_interfaces()` and bind both sides to the intended adapter with `mn_host_on()` / `mn_join_from()`.
- If the host cannot bind its port, choose another port on the host and use that exact port for scan/join.
- If none works, check ordinary OS firewall/router settings with the network administrator. Do not disable managed controls or run covert tunnels.
