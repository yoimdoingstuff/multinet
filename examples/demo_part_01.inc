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
    printf("  %s host <player-name> [room-name] [port]\n", executable);
    printf("  %s join <ipv4-or-hostname> <player-name> [port]\n", executable);
    printf("  %s scan [port]\n\n", executable);
    printf("Host and join run the live demo. Open two terminals and join 127.0.0.1\n");
    printf("to test on one PC, or use the host PC's LAN address from another PC.\n");
    printf("Controls: WASD move the demo player, M starts the match (host),\n");
    printf("          B broadcasts a test message, Q quits.\n");
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