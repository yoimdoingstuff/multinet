/* Populate config with documented defaults. */
void mn_config_init(mn_config *config);

/* Creates an independent networking instance and opens its UDP socket.
   Returns NULL on failure. Multiple instances may coexist in one process. */
mn_instance *mn_create(const mn_config *config);
void mn_destroy(mn_instance *instance);

/* Session lifecycle. max_players includes the host and is clamped to 2..16. */
int mn_host(mn_instance *instance, const char *player_name,
            const char *room_name, uint8_t max_players);
int mn_discover(mn_instance *instance);
void mn_stop_discovery(mn_instance *instance);
size_t mn_get_rooms(const mn_instance *instance, mn_room *rooms, size_t capacity);
int mn_join(mn_instance *instance, const char *ipv4_or_hostname, uint16_t port,
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

/* Convenience status helpers. */
int mn_is_host(const mn_instance *instance);
int mn_is_connected(const mn_instance *instance);
const char *mn_version(void);