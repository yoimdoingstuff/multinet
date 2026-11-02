    mn_get_info(client, &client_info);
    CHECK(client_info.match_started != 0, "client receives match-start event");

    /* Fill the room to its advertised 16-player cap, using separate instances
       in this process to exercise independent client sockets and host slots. */
    for (j = 0; j < MN_MAX_PLAYERS - 2u; ++j) {
        char name[MN_PLAYER_NAME_LENGTH];
        extra_clients[j] = mn_create(&config);
        CHECK(extra_clients[j] != NULL, "create additional player instance");
        snprintf(name, sizeof(name), "Guest%02u", (unsigned)(j + 1u));
        CHECK(mn_join(extra_clients[j], "127.0.0.1", config.port, name),
              "additional client starts joining");
        extra_count++;
    }
    for (iteration = 0; iteration < 250; ++iteration) {
        mn_update(host, 0.01f);
        mn_update(client, 0.01f);
        for (j = 0; j < extra_count; ++j) mn_update(extra_clients[j], 0.01f);
        sleep_ms(4);
    }
    mn_get_info(host, &host_info);
    CHECK(host_info.player_count == MN_MAX_PLAYERS,
          "host supports 16 simultaneous players including the host");
    for (j = 0; j < extra_count; ++j) {
        mn_get_info(extra_clients[j], &client_info);
        CHECK(client_info.status == MN_STATUS_CONNECTED,
              "all additional clients connect");
    }

    for (j = 0; j < extra_count; ++j) mn_leave(extra_clients[j]);
    mn_leave(client);
    for (iteration = 0; iteration < 30; ++iteration) {
        mn_update(host, 0.01f);
        sleep_ms(5);
    }
    mn_get_info(host, &host_info);
    CHECK(host_info.player_count == 1, "host removes players who leave");

    printf("MultiNet tests passed (16-player room, snapshots, messages, match start, leave).\n");
cleanup:
    for (j = 0; j < extra_count; ++j) if (extra_clients[j]) mn_destroy(extra_clients[j]);
    if (client) mn_destroy(client);
    if (host) mn_destroy(host);
    return result;
}
