# MultiNet

**MultiNet** is a reusable C11 multiplayer networking library extracted from the LAN transport built for [Golfgame](https://github.com/yoimdoingstuff/golfgame). It separates generic networking from Golfgame's lobby UI, course loading, physics and ball rendering, then refactors it into a portable instance-based API with a standalone test app.

It targets lightweight desktop games and tools and has no game-engine dependency. Optional AES-256-GCM encryption uses native Windows CNG or OpenSSL Crypto on Unix-like systems.

## Features

- Independent `mn_instance` objects. One process can host a room and connect multiple local test clients.
- UDP host/client rooms, configurable port and application ID.
- OS interface enumeration and explicit interface binding for Ethernet, Wi-Fi, Wi-Fi Direct and Bluetooth PAN IP links.
- IPv4 LAN broadcast discovery, DNS-SD/mDNS service discovery, retrying directed/unicast discovery when broadcasts are filtered, and manual IPv4/hostname joining as a final fallback.
- Up to **16 players per room**, including the host.
- Periodic player snapshots: position, rotation, velocity, score, flags, colour and four application-defined appearance IDs.
- Host-controlled match start, and queued connection, room, player, match, message and error events.
- Application-defined message channels with both best-effort and reliable acknowledged delivery, per-delivery IDs/events, broadcast and host-to-player messaging.
- Non-blocking sockets, timeouts, per-session random join tokens, an optional IPv4 admission filter, persistent device IDs/private secrets, host-approved HMAC challenge-response pairing, AES-256-GCM authenticated packet encryption, fixed-endian serialization, replay checks, ping/latency measurement and host-side rate limits.
- Windows Winsock and POSIX socket backends; CMake build, installable static library, integration tests and interactive demo.

The library transports game state but does not simulate it. Golfgame remains responsible for physics, ball interpolation, scoring and UI.

## Build

Requirements: CMake 3.16+ and a C11 compiler. Visual Studio 2022, GCC and Clang are suitable. Encryption is enabled by default: Windows uses built-in CNG, while Linux/macOS require the OpenSSL Crypto development package. Set `-DMULTINET_ENABLE_ENCRYPTION=OFF` to build without encryption; pairing and room-key APIs are then unavailable.

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

The build produces `multinet` (static library), an interactive demo, `multinet_chat` (reliable one-shot chat example), `multinet_2d_object` (replicated 2D point example) and `multinet_tests` (loopback integration test). On Windows, `multinet_demo.exe` is a native GUI application designed to open by double-click; the terminal version is `multinet_cli_demo.exe`. On Linux, `multinet_demo` remains the terminal demo. Push a version tag such as `v0.6.0` to trigger the release workflow, which verifies version consistency, packages Windows/Linux builds and publishes a GitHub Release with SHA-256 sidecars. Install the library and CMake package with `cmake --install build --prefix install`. A consumer can then use `find_package(MultiNet CONFIG REQUIRED)` and link `MultiNet::MultiNet`. Disable optional targets with `-DMULTINET_BUILD_DEMO=OFF` or `-DMULTINET_BUILD_TESTS=OFF`.

An optional Clang/libFuzzer target exercises malformed packet framing, room-offer parsing and player-state decoding. It is separate from the normal cross-platform build:

```sh
CC=clang cmake -S . -B build-fuzz -DMULTINET_BUILD_FUZZERS=ON -DMULTINET_BUILD_DEMO=OFF -DMULTINET_BUILD_TESTS=OFF
cmake --build build-fuzz --parallel
./build-fuzz/multinet_packet_fuzzer -max_len=1472 -runs=10000
```

## Examples

The small examples use only the public library API and run as separate processes.

For a reliable chat-message exchange, start a host on one PC and join from another:

```sh
./build/multinet_chat host Alice "Hello from Alice"
./build/multinet_chat join 192.168.1.20 Bob "Hello from Bob"
```

The host waits for a client, the client joins the room, and each process sends one reliable message on channel 1. The sample prints received messages and delivery acknowledgement/failure events. An optional final argument sets the UDP port.

For a replicated 2D point, start the host and then a client:

```sh
./build/multinet_2d_object host Alice
./build/multinet_2d_object join 192.168.1.20 Bob
```

Each process moves its own point and prints all replicated player positions once per second. The example uses `mn_player.position.x/y` as the 2D coordinates; the library itself remains renderer- and engine-independent.

## Demo

### Windows graphical demo

Run `multinet_demo.exe` from the build or install `bin` directory. The window lets you host a room, search for rooms, join a selected room or manual IP address, choose or refresh the local network adapter list, view players/events, send a test message, measure latency with Ping, start a match and leave. Select an active Bluetooth PAN or Wi-Fi Direct adapter to bind the room or client to that OS-managed IP link. Pair/connect devices in Windows first; MultiNet does not silently enable radios or create a PAN group.

The GUI timer keeps the network update loop running while the window is open. Keep the app open during play. The small chat and 2D object examples remain command-line samples; on Windows they display a help dialog when launched without arguments.

### Terminal demo

Run the host and client in separate terminals when testing the CLI build:

**Windows PowerShell**

```powershell
.\build\multinet_cli_demo.exe host Alice "Test Room"
.\build\multinet_cli_demo.exe join 127.0.0.1 Bob
```

**Linux**

```sh
./build/multinet_demo host Alice "Test Room"
./build/multinet_demo join 127.0.0.1 Bob
```

For another PC on the same permitted LAN, replace `127.0.0.1` with the host PC's local IPv4 address. The default UDP port is `27890`; the host firewall may need to allow it. The CLI demo does not tunnel traffic or bypass network restrictions.

Controls: **W/A/S/D** move the demo player, **M** starts the match on the host, **B** broadcasts a sample message, and **Q** exits. On Windows use `multinet_cli_demo.exe interfaces`; on Linux use `./build/multinet_demo interfaces`. The other terminal commands are `scan`, `scan-mdns`, `host-on`, `join-from` and `scan-on`. For an adapter-bound unicast probe, fill in the GUI's Host IP box and choose an adapter, or use `mn_discover_at_from()` from the C API. When broadcast discovery is filtered, scan a known host IP directly or skip discovery and join it. Host, join and scan accept custom UDP ports.

The loopback integration test exercises independent instances, player replication, host-side position/velocity bounds and application validation callbacks, best-effort and acknowledged messages in both directions, delivery IDs and per-player ACKs for a 15-recipient broadcast, pending-delivery failure on departure, ping, match start, the full 16-player limit and player departure. This is not a substitute for testing two physical Windows PCs over a real LAN.

## Secure rooms

Encryption must be configured before hosting, joining or discovering rooms over the game UDP socket. Generate a random 32-byte key once with `mn_generate_room_key()`, distribute it to authorized participants using a trusted out-of-band method, then call `mn_set_room_key()` on every instance while it is offline. Never commit the key to source control or print it to logs.

On the host, call `mn_set_pairing_required(host, 1)` after setting the room key. Poll for `MN_EVENT_PAIRING_REQUEST` and show the device ID to the host application's user for approval. Call `mn_approve_pairing(host, event.device_id, 1)` to approve or pass zero to reject. Approval is completed only after the client proves possession of its private device secret with HMAC-SHA256. Persist each approved ID and secret using `mn_get_paired_device_secret()`; persist clients' own identity pair using `mn_get_device_id()`, `mn_get_device_secret()` and `mn_set_device_identity()`.

The room key is shared by every participant, so an authorized key holder can decrypt all room packets. The first-time device secret is carried inside the encrypted JOIN packet; another participant who can capture room traffic and knows the room key could observe that initial secret. This is suitable for private friend/LAN rooms where members are trusted, not a hostile-member or public-internet security boundary. mDNS remains a clear-text discovery protocol and may expose room names, host names and player counts.

See [the API guide](docs/API.md) and [the protocol security notes](docs/PROTOCOL.md) for setup details and limitations.

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

Version 0.6 adds a double-clickable Windows GUI demo and multi-adapter mDNS discovery/address selection. Version 0.5 added AES-256-GCM authenticated UDP packets, persistent device identities, host-approved HMAC challenge-response pairing and paired-device credential storage. Version 0.4 added configurable host-side remote-state bounds and an application validation callback. Version 0.3 added delivery-ID reporting and acknowledgement/failure events for reliable messages, including pending-delivery failures when a recipient leaves. See [`docs/API.md`](docs/API.md) for the full API and Golfgame mapping, [`docs/CONNECTION_FALLBACKS.md`](docs/CONNECTION_FALLBACKS.md) for the discovery/join fallback order, [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for packet format and trust model, and [`docs/GOLFGAME_EXTRACTION.md`](docs/GOLFGAME_EXTRACTION.md) for extraction details.

## Scope and limitations

MultiNet currently supports ordinary IPv4 UDP over operating-system-provided interfaces. It can use Ethernet, Wi-Fi, Wi-Fi Direct or Bluetooth PAN when those systems expose an IPv4 adapter. Discovery includes IPv4 broadcast and directed/unicast probing on usable adapters, plus DNS-SD/mDNS queries and announcements across active IPv4 interfaces. It does not implement native Bluetooth RFCOMM/BLE or create/pair Wi-Fi Direct groups itself. Bluetooth PAN is supported as a normal OS-managed IP adapter when the OS has brought the link up. IPv6, NAT traversal, public matchmaking, multi-hop relaying and covert tunnelling are not included. Optional AES-256-GCM encryption uses a shared 32-byte pre-shared room key that the application must distribute out of band. Every member with that key can decrypt room traffic; this protects against outsiders without the key, not malicious room members. mDNS discovery still exposes room metadata, and the protocol does not yet provide forward secrecy or automated key exchange. Player-state snapshots and `mn_send_message()` custom messages are best-effort UDP datagrams. `mn_send_reliable_message()` adds bounded acknowledgement/retry and duplicate suppression for application events, but it is not durable storage across disconnects. The host relays client-reported state; it is not an authoritative simulation or anti-cheat system. Treat all remote state as untrusted and validate it in the game.

## License

MultiNet is offered under the GNU Lesser General Public License v3 or, at your option, the GNU General Public License v3. See [`LICENSE.LESSER`](LICENSE.LESSER) and [`LICENSE`](LICENSE). The extracted source originated in Golfgame's GPL-3.0 project and has been relicensed for this repository by its maintainer.
