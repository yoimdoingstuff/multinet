# Golfgame extraction notes

## Source extracted

The first implementation was in Golfgame's `src/network/tg_network.h` and `src/network/tg_network.c`. That code provided the original UDP LAN lobby: host, broadcast room discovery, join, periodic player snapshots, match-start signal, leave and timeout handling. The Golfgame integration layer in `src/integration/golf_multiplayer.c` called that transport and then added a game HUD, lobby controls, course-start handling and ball interpolation.

Only the reusable networking responsibilities belong in MultiNet. The Golfgame-specific ImGui panels, course progression, Open-Golf hooks, ball rendering and smoothing remain in Golfgame.

## Why the code was refactored instead of copied verbatim

The extracted prototype used a single static global networking context and serialized packed C structs directly into UDP packets. Those choices made it difficult to host multiple instances in one process, reuse the code in a different game, and guarantee a stable wire layout across compilers and CPU endianness. The library now uses opaque instances, explicit big-endian field encoding, protocol/app identifiers, length validation and OS-backed random session tokens.

A generic player record replaces the golf-specific ball struct. Games map their own state into position, rotation, velocity, score, flags, colour and appearance fields. Application-defined messages use channel IDs and an event queue.

## Old API to new API

| Golfgame `tg_network` | MultiNet |
|---|---|
| `tg_net_init()` | `mn_create()` |
| `tg_net_shutdown()` | `mn_destroy()` |
| `tg_net_host(name, room, max)` | `mn_host(instance, name, room, max)` |
| `tg_net_discover()` | `mn_discover(instance)` |
| `tg_net_get_rooms(out, capacity)` | `mn_get_rooms(instance, out, capacity)` |
| `tg_net_join(host, name)` | `mn_join(instance, host, port, name)` |
| `tg_net_start_match()` | `mn_start_match(instance)` |
| `tg_net_leave()` | `mn_leave(instance)` |
| `tg_net_update(dt)` | `mn_update(instance, dt)` |
| `tg_net_set_local_player(player)` | `mn_set_local_player(instance, player)` |
| `tg_net_get_info(out)` | `mn_get_info(instance, out)` |
| `tg_net_get_players(&count)` | `mn_get_players(instance, out, capacity)` |

`mn_poll_event()` and `mn_send_message()` are new reusable facilities. The old `tg_net_player` can be mapped to `mn_player`; `ball_x/y/z` maps to `position`, `ball_qx/qy/qz/qw` maps to `rotation`, `strokes` maps to `score`, `ball_colour_rgba` maps to `colour_rgba`, and `hat_id` / `trail_id` map to `appearance[0]` / `[1]`.

## Golfgame integration sequence

1. Add MultiNet as a Git submodule, CMake `FetchContent` dependency or vendored source dependency.
2. Replace the `tg_network` target with `MultiNet::MultiNet` (or the exported `multinet` target when using this repository as a subdirectory).
3. Keep one `mn_instance *` owned by the game multiplayer integration layer.
4. Convert the Golfgame player/ball structure into `mn_player` before `mn_set_local_player()`.
5. Call `mn_update()` from the game update loop and translate `mn_get_players()` snapshots into Golfgame remote-ball state.
6. Use `mn_poll_event()` for room/player/match events and `mn_send_message()` for game-specific events that are not part of the replicated player record.
7. Keep game simulation, collision, scoring, rendering and UI in Golfgame. Do not treat a replicated state snapshot as proof that a shot or hole completion is valid.

This repository extraction does not automatically change the Golfgame repository or its build. It establishes MultiNet as the separate library that can be integrated there in a follow-up change.
