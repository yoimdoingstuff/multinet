# MultiNet

**MultiNet** is a reusable C11 multiplayer networking library extracted from the LAN transport built for [Golfgame](https://github.com/yoimdoingstuff/golfgame). It separates generic networking from Golfgame's lobby UI, course loading, physics and ball rendering, then refactors it into a portable instance-based API with a standalone test app.

It targets lightweight desktop games and tools. It has no engine or third-party runtime dependency.

## Features

- Independent `mn_instance` objects. One process can host a room and connect multiple local test clients.
- UDP host/client rooms, configurable port and application ID.
- OS interface enumeration and explicit interface binding for Ethernet, Wi-Fi, Wi-Fi Direct and Bluetooth PAN IP links.
- IPv4 LAN broadcast discovery, DNS-SD/mDNS service discovery, retrying directed/unicast discovery when broadcasts are filtered, and manual IPv4/hostname joining as a final fallback.
- Up to **16 players per room**, including the host.
- Periodic player snapshots: position, rotation, velocity, score, flags, colour and four application-defined appearance IDs.
- Host-controlled match start, and queued connection, room, player, match, message and error events.
- Application-defined message channels with both best-effort and reliable acknowledged delivery, per-delivery IDs/events, broadcast and host-to-player messaging.
- Non-blocking sockets, timeouts, per-session random join tokens, fixed-endian packet serialization, stale-packet sequence checks, ping/latency measurement and basic host-side state/message cooldowns.
- Windows Winsock and POSIX socket backends; CMake build, installable static library, integration tests and interactive demo.

The library transports game state but does not simulate it. Golfgame remains responsible for physics, ball interpolation, scoring and UI.

## Build

Requirements: CMake 3.16+ and a C11 compiler. Visual Studio 2022, GCC and Clang are suitable.

**Linux/macOS**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

**Windows (Visual Studio 2022)**

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The build produces `multinet` (static library), `multinet_demo` (interactive demo) and `multinet_tests` (loopback integration test). Install the library and CMake package with `cmake --install build --prefix install`. A consumer can then use `find_package(MultiNet CONFIG REQUIRED)` and link `MultiNet::MultiNet`. Disable optional targets with `-DMULTINET_BUILD_DEMO=OFF` or `-DMULTINET_BUILD_TESTS=OFF`.

An optional Clang/libFuzzer target exercises malformed packet framing, room-offer parsing and player-state decoding. It is separate from the normal cross-platform build:

```sh
CC=clang cmake -S . -B build-fuzz -DMULTINET_BUILD_FUZZERS=ON -DMULTINET_BUILD_DEMO=OFF -DMULTINET_BUILD_TESTS=OFF
cmake --build build-fuzz --parallel
./build-fuzz/multinet_packet_fuzzer -max_len=1472 -runs=10000
```

## Demo

Open two terminals on one PC. Start the host:

```sh
./build/multinet_demo host Alice "Test Room"
```

Join from the second terminal:

```sh
./build/multinet_demo join 127.0.0.1 Bob
```

For another PC on the same permitted LAN, replace `127.0.0.1` with the host PC's local IPv4 address. The default UDP port is `27890`; the host firewall may need to allow it. The demo does not tunnel traffic or bypass network restrictions.

Controls: **W/A/S/D** move the demo player, **M** starts the match on the host, **B** broadcasts a sample message, and **Q** exits. Use `./build/multinet_demo interfaces` to list local IPv4 adapters. Use `./build/multinet_demo scan-mdns` to discover rooms through DNS-SD/mDNS when the local network supports multicast. `./build/multinet_demo host-on 192.168.1.20 Alice` and `./build/multinet_demo join-from 192.168.1.30 192.168.1.20 Bob` bind each side to a chosen interface; `./build/multinet_demo scan-on 192.168.1.30` scans through that adapter. Discover nearby rooms with `./build/multinet_demo scan`, which automatically tries global LAN broadcast, per-interface subnet broadcast and mDNS/DNS-SD. When broadcast discovery is filtered, probe a known host directly with `./build/multinet_demo scan 27890 192.168.1.10`, or skip discovery and join directly with `./build/multinet_demo join 192.168.1.10 Bob`. Host, join and scan accept custom UDP ports.

The loopback integration test exercises independent instances, player replication, host-side position/velocity bounds and application validation callbacks, best-effort and acknowledged messages in both directions, delivery IDs and per-player ACKs for a 15-recipient broadcast, pending-delivery failure on departure, ping, match start, the full 16-player limit and player departure. This is not a substitute for testing two physical Windows PCs over a real LAN.

## Quick API example

```c
#include "multinet.h"

int main(void) {
    mn_config config;
    mn_instance *net;
    mn_player local = {0};

    mn_config_init(&config);
    net = mn_create(&config);
    if (!net) return 1;
    if (!mn_host(net, "Host", "My Room", MN_MAX_PLAYERS)) {
        mn_destroy(net);
        return 1;
    }

    local.colour_rgba = 0x35C7B8FFu;
    local.rotation.w = 1.0f;
    for (;;) {
        (void)mn_set_local_player(net, &local);
        mn_update(net, 1.0f / 60.0f);
        /* Read mn_get_players(), mn_get_info() and mn_poll_event(). */
    }
    mn_destroy(net);
    return 0;
}
```

Version 0.4 adds configurable host-side remote-state bounds and an application validation callback. Version 0.3 added delivery-ID reporting and acknowledgement/failure events for reliable messages, including pending-delivery failures when a recipient leaves. See [`docs/API.md`](docs/API.md) for the full API and Golfgame mapping, [`docs/CONNECTION_FALLBACKS.md`](docs/CONNECTION_FALLBACKS.md) for the discovery/join fallback order, [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for packet format and trust model, and [`docs/GOLFGAME_EXTRACTION.md`](docs/GOLFGAME_EXTRACTION.md) for extraction details.

## Scope and limitations

MultiNet currently supports ordinary IPv4 UDP over operating-system-provided interfaces. It can use Ethernet, Wi-Fi, Wi-Fi Direct or Bluetooth PAN when those systems expose an IPv4 adapter. Discovery includes IPv4 broadcast, directed/unicast probing and DNS-SD/mDNS. It does not implement raw Bluetooth RFCOMM/BLE or create/pair Wi-Fi Direct groups itself. IPv6, NAT traversal, public matchmaking, encryption, multi-hop relaying and covert tunnelling are not included. Player-state snapshots and `mn_send_message()` custom messages are best-effort UDP datagrams. `mn_send_reliable_message()` adds bounded acknowledgement/retry and duplicate suppression for application events, but it is not durable storage across disconnects. The host relays client-reported state; it is not an authoritative simulation or anti-cheat system. Treat all remote state as untrusted and validate it in the game.

## License

MultiNet is offered under the GNU Lesser General Public License v3 or, at your option, the GNU General Public License v3. See [`LICENSE.LESSER`](LICENSE.LESSER) and [`LICENSE`](LICENSE). The extracted source originated in Golfgame's GPL-3.0 project and has been relicensed for this repository by its maintainer.
