/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet.h"

#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
typedef SOCKET mn_socket;
typedef int mn_socklen;
#define MN_BAD_SOCKET INVALID_SOCKET
#define mn_close_socket closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int mn_socket;
typedef socklen_t mn_socklen;
#define MN_BAD_SOCKET (-1)
#define mn_close_socket close
#endif

#define MN_MAGIC 0x4D4E4554u /* "MNET" */
#define MN_PROTOCOL_VERSION 1u
#define MN_HEADER_SIZE 16u
#define MN_MAX_DATAGRAM_SIZE 1472u
#define MN_EVENT_QUEUE_CAPACITY 128u
#define MN_WIRE_PLAYER_SIZE 88u
#define MN_OFFER_SIZE (2u + MN_ROOM_NAME_LENGTH + MN_PLAYER_NAME_LENGTH)
#define MN_WELCOME_SIZE (4u + 8u + 1u + MN_ROOM_NAME_LENGTH)
#define MN_CONNECT_TIMEOUT 10.0f

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(float) == 4, "MultiNet requires 32-bit floats");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "MultiNet requires IEEE-754 binary32 floats");
#endif

enum mn_packet_type {
    MN_PKT_DISCOVER = 1,
    MN_PKT_OFFER,
    MN_PKT_JOIN,
    MN_PKT_WELCOME,
    MN_PKT_STATE,
    MN_PKT_SNAPSHOT,
    MN_PKT_LEAVE,
    MN_PKT_START,
    MN_PKT_MESSAGE
};

typedef struct mn_peer {
    int active;
    uint32_t id;
    uint64_t token;
    float silence;
    struct sockaddr_in address;
    mn_player player;
} mn_peer;

struct mn_instance {
    int ready;
    mn_socket socket;
    mn_config config;
    mn_info info;
    mn_player local_player;
    mn_player players[MN_MAX_PLAYERS];
    mn_room rooms[MN_MAX_ROOMS];
    size_t room_count;
    mn_peer peers[MN_MAX_PLAYERS];
    struct sockaddr_in server;
    uint64_t session_token;
    uint32_t next_player_id;
    uint16_t local_port;
    float discovery_left;
    float discovery_tick;
    float heartbeat_tick;
    float snapshot_tick;
    float connect_elapsed;
    float server_silence;
    mn_event events[MN_EVENT_QUEUE_CAPACITY];
    size_t event_head;
    size_t event_count;
};

static void mn_copy_text(char *destination, size_t capacity, const char *source) {
    size_t length;
    if (!destination || capacity == 0) return;
    if (!source) source = "";
    length = strlen(source);
    if (length >= capacity) length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void mn_push_event(mn_instance *instance, const mn_event *event) {
    size_t slot;
    if (!instance || !event) return;
    if (instance->event_count == MN_EVENT_QUEUE_CAPACITY) {
        instance->event_head = (instance->event_head + 1u) % MN_EVENT_QUEUE_CAPACITY;
        instance->event_count--;
    }
    slot = (instance->event_head + instance->event_count) % MN_EVENT_QUEUE_CAPACITY;
    instance->events[slot] = *event;
    instance->event_count++;
}

static void mn_emit_simple_event(mn_instance *instance, mn_event_type type,
                                 const char *message, uint32_t player_id) {
    mn_event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.player_id = player_id;
    mn_copy_text(event.text, sizeof(event.text), message);
    mn_push_event(instance, &event);
}
