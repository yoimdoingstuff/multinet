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
    MN_EVENT_ERROR
} mn_event_type;

typedef struct mn_event {
    mn_event_type type;
    uint32_t player_id;
    uint16_t channel;
    uint16_t data_size;
    mn_player player;
    uint8_t data[MN_MAX_MESSAGE_BYTES];
    char text[160];
} mn_event;