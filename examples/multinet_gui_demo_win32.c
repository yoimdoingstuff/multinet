static void refresh_players(void) {
    mn_player players[MN_MAX_PLAYERS];
    size_t count, i;
    char line[128];
    SendMessageA(g_players_list, LB_RESETCONTENT, 0, 0);
    count = mn_get_players(g_network, players, MN_MAX_PLAYERS);
    if (count > MN_MAX_PLAYERS) count = MN_MAX_PLAYERS;
    for (i = 0; i < count; ++i) {
        (void)snprintf(line, sizeof(line), "#%u  %s  score %d",
                       (unsigned)players[i].id, players[i].name,
                       (int)players[i].score);
        SendMessageA(g_players_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
}