/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#ifndef MULTINET_H
#define MULTINET_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MN_VERSION_MAJOR 0
#define MN_VERSION_MINOR 3
#define MN_VERSION_PATCH 0
#define MN_VERSION_STRING "0.3.0"
#define MN_MAX_PLAYERS 16u
#define MN_MAX_ROOMS 32u
#define MN_PLAYER_NAME_LENGTH 24u
#define MN_ROOM_NAME_LENGTH 32u
#define MN_MAX_MESSAGE_BYTES 768u
#define MN_MAX_RELIABLE_PENDING 64u
#define MN_DEFAULT_PORT 27890u
#define MN_DEFAULT_APP_ID 0x4D4E5431u /* "MNT1" */

typedef struct mn_instance mn_instance;

typedef enum mn_role {
    MN_ROLE_NONE = 0,
    MN_ROLE_HOST = 1,
    MN_ROLE_CLIENT = 2
} mn_role;

typedef enum mn_status {
    MN_STATUS_OFFLINE = 0,
    MN_STATUS_SEARCHING = 1,
    MN_STATUS_HOSTING = 2,
    MN_STATUS_CONNECTING = 3,
    MN_STATUS_CONNECTED = 4
} mn_status;

typedef struct mn_vec3 {
    float x, y, z;
} mn_vec3;

typedef struct mn_quat {
    float x, y, z, w;
} mn_quat;

/* Generic replicated player data. A game may interpret appearance[] and
   flags however it likes. Keep cosmetic/appearance fields visual-only. */
typedef struct mn_player {
    uint32_t id;
    char name[MN_PLAYER_NAME_LENGTH];
    mn_vec3 position;
    mn_quat rotation;
    mn_vec3 velocity;
    uint32_t colour_rgba;       /* Packed 0xRRGGBBAA. */
    int32_t score;
    uint32_t flags;             /* Application-defined bit field. */
    uint16_t appearance[4];     /* Application-defined cosmetic identifiers. */
} mn_player;

typedef struct mn_room {
    char name[MN_ROOM_NAME_LENGTH];
    char host_name[MN_PLAYER_NAME_LENGTH];
    char address[48];           /* Numeric IPv4 address. */
    uint16_t port;
    uint8_t player_count;
    uint8_t max_players;
    uint32_t room_id;
    uint32_t app_id;
} mn_room;

typedef struct mn_info {
    mn_status status;
    mn_role role;
    uint32_t app_id;
    uint32_t local_player_id;
    uint32_t room_id;
    uint8_t player_count;
    uint8_t max_players;
    uint8_t match_started;
    uint16_t local_port;
    char room_name[MN_ROOM_NAME_LENGTH];
    char local_name[MN_PLAYER_NAME_LENGTH];
    char last_error[160];
    float ping_ms;
    uint8_t ping_available;
    uint8_t ping_pending;
    uint8_t mdns_available;
} mn_info;

typedef struct mn_config {
    uint16_t port;              /* Host/discovery port; defaults to 27890. */
    uint32_t app_id;            /* Separates unrelated applications on a LAN. */
    uint8_t max_players;        /* Maximum including the host; defaults to 16. */
    uint8_t enable_broadcast;   /* Set to 0 to disable LAN broadcast discovery. */
    float discovery_seconds;   /* Discovery window; defaults to 3 seconds. */
    float heartbeat_seconds;   /* Player state/snapshot cadence; defaults to 0.10. */
    float timeout_seconds;     /* Peer timeout; defaults to 8 seconds. */
} mn_config;

typedef enum mn_event_type {
    MN_EVENT_NONE = 0,
    MN_EVENT_CONNECTED,
    MN_EVENT_DISCONNECTED,
    MN_EVENT_PLAYER_JOINED,
    MN_EVENT_PLAYER_LEFT,
    MN_EVENT_ROOM_FOUND,
    MN_EVENT_MATCH_STARTED,
    MN_EVENT_MESSAGE,
    MN_EVENT_ERROR,
    MN_EVENT_MESSAGE_ACKNOWLEDGED,
    MN_EVENT_MESSAGE_FAILED
} mn_event_type;


#define MN_INTERFACE_NAME_LENGTH 64u
#define MN_INTERFACE_DESCRIPTION_LENGTH 128u

typedef enum mn_interface_type {
    MN_INTERFACE_OTHER = 0,
    MN_INTERFACE_LOOPBACK = 1,
    MN_INTERFACE_ETHERNET = 2,
    MN_INTERFACE_WIFI = 3,
    MN_INTERFACE_WIFI_DIRECT = 4,
    MN_INTERFACE_BLUETOOTH_PAN = 5
} mn_interface_type;

typedef struct mn_interface {
    char name[MN_INTERFACE_NAME_LENGTH];
    char description[MN_INTERFACE_DESCRIPTION_LENGTH];
    char address[48];
    char netmask[48];
    char broadcast[48];
    uint32_t index;
    mn_interface_type type;
} mn_interface;

typedef struct mn_event {
    mn_event_type type;
    uint32_t player_id;
    uint16_t channel;
    uint16_t data_size;
    mn_player player;
    uint8_t data[MN_MAX_MESSAGE_BYTES];
    char text[160];
    uint32_t delivery_id; /* Reliable application-message ID, zero otherwise. */
} mn_event;

/* Populate config with documented defaults. */
void mn_config_init(mn_config *config);

/* Creates an independent networking instance and opens its UDP socket.
   Returns NULL on failure. Multiple instances may coexist in one process. */
mn_instance *mn_create(const mn_config *config);
void mn_destroy(mn_instance *instance);

/* Session lifecycle. max_players includes the host and is clamped to 2..16. */
int mn_host(mn_instance *instance, const char *player_name,
            const char *room_name, uint8_t max_players);
/* Host on one IPv4 interface, including a Bluetooth PAN or Wi-Fi Direct
   adapter exposed by the operating system. */
int mn_host_on(mn_instance *instance, const char *player_name,
               const char *room_name, uint8_t max_players,
               const char *local_ipv4);
int mn_discover(mn_instance *instance);
/* Try IPv4 global/subnet broadcasts and DNS-SD/mDNS together. */
int mn_discover_auto(mn_instance *instance);
/* DNS-SD/mDNS service discovery on the local link. */
int mn_discover_mdns(mn_instance *instance);
/* Unicast discovery fallback for networks where broadcast is filtered. */
int mn_discover_at(mn_instance *instance, const char *ipv4_or_hostname, uint16_t port);
/* Enumerate OS-managed IPv4 interfaces. Returns the total available count. */
size_t mn_get_interfaces(mn_interface *interfaces, size_t capacity);
/* Send discovery on one selected interface's IPv4 directed-broadcast address. */
int mn_discover_on(mn_instance *instance, const mn_interface *network_interface);
void mn_stop_discovery(mn_instance *instance);
size_t mn_get_rooms(const mn_instance *instance, mn_room *rooms, size_t capacity);
int mn_join(mn_instance *instance, const char *ipv4_or_hostname, uint16_t port,
            const char *player_name);
/* Bind the client socket to a chosen local adapter before joining the host. */
int mn_join_from(mn_instance *instance, const char *local_ipv4,
                 const char *ipv4_or_hostname, uint16_t port,
                 const char *player_name);
int mn_join_room(mn_instance *instance, const mn_room *room,
                 const char *player_name);
int mn_start_match(mn_instance *instance); /* Host only. */
void mn_leave(mn_instance *instance);

/* Call regularly from the application's main/update loop. dt_seconds is the
   elapsed time since the previous call. Network I/O is non-blocking. */
void mn_update(mn_instance *instance, float dt_seconds);

/* Replicated state and readback. These functions copy data into caller-owned
   memory; returned pointers never refer to mutable internal storage. */
int mn_set_local_player(mn_instance *instance, const mn_player *player);
void mn_get_info(const mn_instance *instance, mn_info *out_info);
size_t mn_get_players(const mn_instance *instance, mn_player *players, size_t capacity);
int mn_get_player(const mn_instance *instance, uint32_t player_id, mn_player *out_player);

/* Read queued lifecycle/message/error events. Returns 1 when an event was
   copied, 0 when the queue is empty. Events are consumed in FIFO order. */
int mn_poll_event(mn_instance *instance, mn_event *out_event);

/* Application-defined UDP message channels. target_player_id=0 broadcasts.
   A client may send only to the host (target 0); the host relays it to peers.
   Messages are best-effort datagrams, limited to MN_MAX_MESSAGE_BYTES. */
int mn_send_message(mn_instance *instance, uint32_t target_player_id,
                    uint16_t channel, const void *data, size_t size);
/* Queued, acknowledged application message with automatic retransmission.
   Clients send to the host (target 0); hosts may target one player or broadcast. */
int mn_send_reliable_message(mn_instance *instance, uint32_t target_player_id,
                             uint16_t channel, const void *data, size_t size);
/* Extended form returns a delivery ID. Match it with MN_EVENT_MESSAGE_ACKNOWLEDGED
   or MN_EVENT_MESSAGE_FAILED. On a host broadcast, each recipient is ACKed
   independently and event.player_id identifies that recipient. Client-to-host
   acknowledgement events use player_id 0 to identify the host. */
int mn_send_reliable_message_ex(mn_instance *instance, uint32_t target_player_id,
                                uint16_t channel, const void *data, size_t size,
                                uint32_t *out_delivery_id);

/* Convenience status helpers. */
int mn_is_host(const mn_instance *instance);
int mn_is_connected(const mn_instance *instance);
/* Measure round-trip latency: clients pass target 0 to ping the host; the host
   passes a connected player ID to ping that player. Read ping_ms in mn_info. */
int mn_ping(mn_instance *instance, uint32_t target_player_id);

const char *mn_version(void);

#ifdef __cplusplus
}
#endif
#endif /* MULTINET_H */
