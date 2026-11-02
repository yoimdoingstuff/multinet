
        }
    }

    if (instance->info.role == MN_ROLE_HOST) {
        for (i = 0; i < MN_MAX_PLAYERS; ++i) {
            if (instance->peers[i].active) {
                instance->peers[i].silence += dt;
                if (instance->peers[i].silence > instance->config.timeout_seconds) {
                    mn_remove_peer(instance, &instance->peers[i], "Player timed out");
                    mn_broadcast_snapshot(instance);
                }
            }
        }
        instance->snapshot_tick -= dt;
        if (instance->snapshot_tick <= 0.0f) {
            mn_broadcast_snapshot(instance);
            instance->snapshot_tick = instance->config.heartbeat_seconds;
        }
    }
}

int mn_is_host(const mn_instance *instance) {
    return instance && instance->info.role == MN_ROLE_HOST &&
           instance->info.status == MN_STATUS_HOSTING;
}
int mn_is_connected(const mn_instance *instance) {
    return instance && (instance->info.status == MN_STATUS_CONNECTED ||
        instance->info.status == MN_STATUS_HOSTING);
}
const char *mn_version(void) { return MN_VERSION_STRING; }
