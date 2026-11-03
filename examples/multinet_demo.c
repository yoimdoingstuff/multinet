/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
static void sleep_ms(unsigned int milliseconds) { Sleep(milliseconds); }
static int read_key(void) { return _kbhit() ? _getch() : -1; }
static void terminal_begin(void) { }
static void terminal_end(void) { }
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
static struct termios original_terminal;
static int terminal_changed;
static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}
static void terminal_begin(void) {
    struct termios raw;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original_terminal) != 0) return;
    raw = original_terminal;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) terminal_changed = 1;
}
static void terminal_end(void) {
    if (terminal_changed) tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
    terminal_changed = 0;
}
static int read_key(void) {
    fd_set read_set;
    struct timeval timeout;
    unsigned char value;
    int result;
    FD_ZERO(&read_set);
    FD_SET(STDIN_FILENO, &read_set);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    result = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
    if (result <= 0 || read(STDIN_FILENO, &value, 1) != 1) return -1;
    return (int)value;
}
#endif

static double monotonic_seconds(void) {
    struct timespec value;
    timespec_get(&value, TIME_UTC);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static unsigned short parse_port(const char *text, unsigned short fallback) {
    char *end = NULL;
    long value;
    if (!text || !text[0]) return fallback;
    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 1 || value > 65535) return fallback;
    return (unsigned short)value;
}

static void print_usage(const char *executable) {
    printf("MultiNet %s demo\n\n", mn_version());
    printf("Usage:\n");
    printf("  %s interfaces\n", executable);
    printf("  %s host <player-name> [room-name] [port]\n", executable);
    printf("  %s host-on <local-ipv4> <player-name> [room-name] [port]\n", executable);
    printf("  %s join <ipv4-or-hostname> <player-name> [port]\n", executable);
    printf("  %s join-from <local-ipv4> <host-address> <player-name> [port]\n", executable);
    printf("  %s scan [port] [host-address]\n", executable);
    printf("  %s scan-on <local-ipv4> [port]\n\n", executable);
    printf("Host and join run the live demo. Open two terminals and join 127.0.0.1\n");
    printf("to test on one PC, or use the host PC's LAN address from another PC.\n");
    printf("Use 'interfaces' to list Ethernet, Wi-Fi, Wi-Fi Direct and Bluetooth PAN IP adapters.\n");
    printf("Use host-on/join-from/scan-on to select one adapter explicitly.\n");
    printf("Bluetooth PAN and Wi-Fi Direct must first be connected in the operating system.\n");
    printf("When broadcast discovery is filtered, scan with a host address or join directly.\n");
    printf("Controls: WASD move the demo player, M starts the match (host),\n");
    printf("          B broadcasts a test message, Q quits.\n");
}


static const char *interface_type_name(mn_interface_type type) {
    switch (type) {
        case MN_INTERFACE_LOOPBACK: return "loopback";
        case MN_INTERFACE_ETHERNET: return "ethernet";
        case MN_INTERFACE_WIFI: return "wifi";
        case MN_INTERFACE_WIFI_DIRECT: return "wi-fi-direct";
        case MN_INTERFACE_BLUETOOTH_PAN: return "bluetooth-pan";
        default: return "other";
    }
}

static void print_interfaces(void) {
    mn_interface interfaces[64];
    size_t count = mn_get_interfaces(interfaces, 64);
    size_t i;
    printf("Local IPv4 interfaces (%zu):\\n", count);
    for (i = 0; i < count && i < 64; ++i) {
        printf("  %-16s %-14s %-16s mask %-16s broadcast %s\\n",
               interface_type_name(interfaces[i].type),
               interfaces[i].name, interfaces[i].address,
               interfaces[i].netmask[0] ? interfaces[i].netmask : "-",
               interfaces[i].broadcast[0] ? interfaces[i].broadcast : "-");
    }
    printf("\\nBluetooth PAN / Wi-Fi Direct appear here only after the OS has connected them.\\n");
}

static int find_interface(const char *address, mn_interface *out_interface) {
    mn_interface interfaces[64];
    size_t count = mn_get_interfaces(interfaces, 64);
    size_t i;
    if (!address || !out_interface) return 0;
    for (i = 0; i < count && i < 64; ++i) {
        if (strcmp(interfaces[i].address, address) == 0) {
            *out_interface = interfaces[i];
            return 1;
        }
    }
    return 0;
}

static void print_rooms(mn_instance *network) {
    mn_room rooms[MN_MAX_ROOMS];
    size_t count = mn_get_rooms(network, rooms, MN_MAX_ROOMS);
    size_t i;
    printf("\nRooms discovered: %zu\n", count);
    for (i = 0; i < count && i < MN_MAX_ROOMS; ++i) {
        printf("  %-28s host %-20s %u/%u  %s:%u\n",
               rooms[i].name, rooms[i].host_name,
               (unsigned)rooms[i].player_count, (unsigned)rooms[i].max_players,
               rooms[i].address, (unsigned)rooms[i].port);
    }
}

static void drain_events(mn_instance *network) {
    mn_event event;
    while (mn_poll_event(network, &event)) {
        switch (event.type) {
            case MN_EVENT_CONNECTED:
                printf("\n[connected] %s\n", event.text);
                break;
            case MN_EVENT_DISCONNECTED:
                printf("\n[disconnected] %s\n", event.text);
                break;
            case MN_EVENT_PLAYER_JOINED:
                printf("\n[player joined] %s (#%u)\n", event.player.name,
                       (unsigned)event.player_id);
                break;
            case MN_EVENT_PLAYER_LEFT:
                printf("\n[player left] #%u: %s\n", (unsigned)event.player_id,
                       event.text);
                break;
            case MN_EVENT_ROOM_FOUND:
                printf("\n[room found] %s\n", event.text);
                break;
            case MN_EVENT_MATCH_STARTED:

                printf("\n[match started] %s\n", event.text);
                break;
            case MN_EVENT_MESSAGE:
                printf("\n[message] from player #%u on channel %u: ",
                       (unsigned)event.player_id, (unsigned)event.channel);
                if (event.data_size) fwrite(event.data, 1, event.data_size, stdout);
                putchar('\n');
                break;
            case MN_EVENT_ERROR:
                printf("\n[network error] %s\n", event.text);
                break;
            default:
                break;
        }
    }
}

static int run_scan(const mn_config *config, const char *target_host,
                    const char *local_ipv4) {
    mn_instance *network = mn_create(config);
    double start, previous;
    if (!network) {
        fprintf(stderr, "Could not initialise MultiNet. Check socket permissions/firewall.\n");
        return 1;
    }
    {
        mn_interface selected_interface;
        int started;
        if (local_ipv4) {
            if (!find_interface(local_ipv4, &selected_interface)) {
                fprintf(stderr, "No local IPv4 interface has address %s. Use 'interfaces' to list adapters.\n",
                        local_ipv4);
                mn_destroy(network);
                return 1;
            }
            started = mn_discover_on(network, &selected_interface);
        } else if (target_host) {
            started = mn_discover_at(network, target_host, config->port);
        } else {
            started = mn_discover(network);
        }
        if (!started) {
            mn_info info;
            mn_get_info(network, &info);
            fprintf(stderr, "Discovery failed: %s\n", info.last_error);
            mn_destroy(network);
            return 1;
        }
    }
    printf("Searching %s for MultiNet rooms on UDP port %u...\n",
           local_ipv4 ? local_ipv4 : (target_host ? target_host : "the local network"),
           (unsigned)config->port);
    start = monotonic_seconds();
    previous = start;
    while (monotonic_seconds() - start < config->discovery_seconds + 0.2) {
        double now = monotonic_seconds();
        mn_update(network, (float)(now - previous));
        previous = now;
        drain_events(network);
        sleep_ms(20);
    }
    print_rooms(network);
    mn_destroy(network);
    return 0;
}

static int run_session(const mn_config *config, int hosting,
                       const char *local_ipv4, const char *address,
                       const char *player_name, const char *room_name,
                       uint8_t max_players) {
    mn_instance *network = mn_create(config);
    mn_player local;
    mn_info info;
    double previous, last_print;
    int running = 1;
    if (!network) {
        fprintf(stderr, "Could not initialise MultiNet. Check socket permissions/firewall.\n");
        return 1;
    }
    if (hosting) {
        if (!(local_ipv4 ? mn_host_on(network, player_name, room_name, max_players, local_ipv4)
                         : mn_host(network, player_name, room_name, max_players))) {
            mn_get_info(network, &info);
            fprintf(stderr, "Host failed: %s\n", info.last_error);
            mn_destroy(network);
            return 1;
        }
        printf("Hosting '%s' as %s on UDP %u.\n", room_name, player_name,
               (unsigned)config->port);
    } else {
        if (!(local_ipv4 ? mn_join_from(network, local_ipv4, address, config->port, player_name)
                         : mn_join(network, address, config->port, player_name))) {
            mn_get_info(network, &info);
            fprintf(stderr, "Join failed: %s\n", info.last_error);
            mn_destroy(network);
            return 1;
        }
        printf("Joining %s:%u as %s...\n", address, (unsigned)config->port,
               player_name);
    }

    memset(&local, 0, sizeof(local));
    local.colour_rgba = hosting ? 0x35C7B8FFu : 0xFFB84DFFu;
    local.rotation.w = 1.0f;
    printf("Controls: WASD move, M start match (host), B send message, Q quit.\n");
    printf("Press keys directly in this window.\n\n");
    terminal_begin();
    previous = monotonic_seconds();
    last_print = previous;
    while (running) {
        double now = monotonic_seconds();
        float dt = (float)(now - previous);
        int key;
        mn_player players[MN_MAX_PLAYERS];
        size_t count, i;
        previous = now;
        while ((key = read_key()) >= 0) {
            if (key == 'q' || key == 'Q') running = 0;
            else if (key == 'w' || key == 'W') local.position.y += 0.5f;
            else if (key == 's' || key == 'S') local.position.y -= 0.5f;
            else if (key == 'a' || key == 'A') local.position.x -= 0.5f;
            else if (key == 'd' || key == 'D') local.position.x += 0.5f;
            else if ((key == 'm' || key == 'M') && hosting) {
                if (!mn_start_match(network)) printf("\nCould not start match.\n");
            } else if (key == 'b' || key == 'B') {
                static const char message[] = "Hello from the MultiNet demo!";
                (void)mn_send_message(network, 0, 1, message, sizeof(message) - 1);
            }
        }
        (void)mn_set_local_player(network, &local);
        mn_update(network, dt);
        mn_get_info(network, &info);
        drain_events(network);
        if (now - last_print >= 1.0) {


            mn_get_info(network, &info);
            count = mn_get_players(network, players, MN_MAX_PLAYERS);
            printf("\n[%s] %s | players %u/%u | local ball (%.1f, %.1f, %.1f)\n",
                   info.role == MN_ROLE_HOST ? "HOST" : "CLIENT",
                   info.status == MN_STATUS_HOSTING ? "hosting" :
                   info.status == MN_STATUS_CONNECTED ? "connected" :
                   info.status == MN_STATUS_CONNECTING ? "connecting" : "offline",
                   (unsigned)info.player_count, (unsigned)info.max_players,
                   local.position.x, local.position.y, local.position.z);
            for (i = 0; i < count && i < MN_MAX_PLAYERS; ++i) {
                printf("  #%u %-22s pos=(%6.1f, %6.1f, %6.1f) score=%d%s\n",
                       (unsigned)players[i].id, players[i].name,
                       players[i].position.x, players[i].position.y,
                       players[i].position.z, players[i].score,
                       (players[i].flags & 1u) ? "  [moving]" : "");
            }
            if (info.last_error[0]) printf("  last error: %s\n", info.last_error);
            fflush(stdout);
            last_print = now;
        }
        sleep_ms(16);
        if (info.status == MN_STATUS_OFFLINE && !hosting) {
            mn_get_info(network, &info);
            if (info.status == MN_STATUS_OFFLINE && info.last_error[0]) running = 0;
        }
    }
    terminal_end();
    mn_leave(network);
    mn_destroy(network);
    printf("\nMultiNet demo closed.\n");
    return 0;
}

int main(int argc, char **argv) {
    mn_config config;
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return argc < 2 ? 0 : 0;
    }
    mn_config_init(&config);
    if (strcmp(argv[1], "scan") == 0) {
        config.port = argc >= 3 ? parse_port(argv[2], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_scan(&config, argc >= 4 ? argv[3] : NULL, NULL);
    }
    if (strcmp(argv[1], "scan-on") == 0 && argc >= 3) {
        config.port = argc >= 4 ? parse_port(argv[3], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_scan(&config, NULL, argv[2]);
    }
    if (strcmp(argv[1], "interfaces") == 0) {
        print_interfaces();
        return 0;
    }
    if (strcmp(argv[1], "host") == 0) {
        const char *name = argc >= 3 ? argv[2] : "Host";
        const char *room = argc >= 4 ? argv[3] : "MultiNet Test Room";
        config.port = argc >= 5 ? parse_port(argv[4], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_session(&config, 1, NULL, NULL, name, room, MN_MAX_PLAYERS);
    }
    if (strcmp(argv[1], "host-on") == 0 && argc >= 4) {
        const char *local_ipv4 = argv[2];
        const char *name = argv[3];
        const char *room = argc >= 5 ? argv[4] : "MultiNet Interface Test";
        config.port = argc >= 6 ? parse_port(argv[5], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_session(&config, 1, local_ipv4, NULL, name, room, MN_MAX_PLAYERS);
    }
    if (strcmp(argv[1], "join") == 0 && argc >= 4) {
        const char *address = argv[2];
        const char *name = argv[3];
        config.port = argc >= 5 ? parse_port(argv[4], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_session(&config, 0, NULL, address, name, NULL, MN_MAX_PLAYERS);
    }
    if (strcmp(argv[1], "join-from") == 0 && argc >= 5) {
        const char *local_ipv4 = argv[2];
        const char *address = argv[3];
        const char *name = argv[4];
        config.port = argc >= 6 ? parse_port(argv[5], MN_DEFAULT_PORT) : MN_DEFAULT_PORT;
        return run_session(&config, 0, local_ipv4, address, name, NULL, MN_MAX_PLAYERS);
    }
    print_usage(argv[0]);
    return 2;
}