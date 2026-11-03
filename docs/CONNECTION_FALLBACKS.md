# Connection fallbacks

MultiNet deliberately does not depend on one discovery method. Local networks differ, and broadcast packets are often filtered even when normal UDP unicast is allowed.

Use these connection paths in order:

1. **Automatic LAN broadcast discovery.** Call `mn_discover()`, continue calling `mn_update()`, and read `mn_get_rooms()`. This is the easiest option when the local subnet forwards UDP broadcast.
2. **Directed/unicast discovery.** When broadcast is disabled or no rooms appear, call `mn_discover_at(instance, known_host, port)`. MultiNet sends a normal discovery request directly to that IPv4 address or hostname and retries it for the discovery window. This requires knowing the host address but does not require subnet broadcast.
3. **Manual direct join.** When room discovery is blocked entirely, call `mn_join(instance, host, port, player_name)`. The host still needs to be reachable on its configured UDP port.
4. **Port fallback.** The host and client can choose a different UDP port in `mn_config.port` or the join argument. This helps with a local port conflict, but both sides must agree on the selected port and the firewall must permit it.

There is no hidden relay or tunnel. These are ordinary UDP connection attempts over the network the user is already allowed to use. A firewall or network policy may block all of them; MultiNet reports a timeout/error instead of trying to bypass that restriction.

## Using Bluetooth PAN, Wi-Fi Direct, hotspots and Ethernet

MultiNet uses ordinary IP networking. It can use Ethernet, Wi-Fi, phone hotspots, USB tethering, Wi-Fi Direct and Bluetooth Personal Area Network (PAN) when the operating system has created an IP interface for that connection. Pair/connect the devices through the normal OS network settings first, then call `mn_get_interfaces()` to list their IPv4 addresses and adapter types. Bind the host with `mn_host_on()` and the client with `mn_join_from()` to select that interface instead of relying on whichever route the OS happens to choose.

This is Bluetooth PAN networking, not direct Bluetooth Low Energy advertising or RFCOMM. MultiNet does not silently pair devices or toggle radios. Bluetooth PAN and Wi-Fi Direct availability depends on OS version, drivers, device support and administrator policy. If they do not create usable IPv4 addresses, use ordinary Wi-Fi/Ethernet or a supported hotspot.

## Troubleshooting order

- Confirm both devices are on the same permitted LAN and that the host is actually hosting.
- Try automatic discovery.
- If the scan shows no room, use a known host IPv4 address with directed discovery.
- If directed discovery gets no offer, try direct join to the same host and port.
- If the device has multiple adapters, enumerate them with `mn_get_interfaces()` and bind both sides to the intended adapter with `mn_host_on()` / `mn_join_from()`.
- If the host cannot bind its port, choose another port on the host and use that exact port for scan/join.
- If none works, check ordinary OS firewall/router settings with the network administrator. Do not disable managed controls or run covert tunnels.
