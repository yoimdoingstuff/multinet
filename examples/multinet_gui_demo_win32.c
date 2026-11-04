/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include "multinet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_NAME       1001
#define ID_ROOM       1002
#define ID_PORT       1003
#define ID_INTERFACE  1004
#define ID_HOST       1005
#define ID_HOST_BTN   1010
#define ID_SCAN_BTN   1011
#define ID_JOIN_BTN   1012
#define ID_LEAVE_BTN  1013
#define ID_START_BTN  1014
#define ID_SEND_TEXT  1015
#define ID_SEND_BTN   1016
#define ID_REFRESH_BTN 1017
#define ID_PING_BTN    1018
#define ID_ROOMS      1020
#define ID_PLAYERS    1021
#define ID_LOG        1022
#define ID_STATUS     1023

static HINSTANCE g_instance_handle;
static HWND g_window;
static HWND g_name, g_room, g_port, g_interface, g_host;
static HWND g_rooms_list, g_players_list, g_log, g_status;
static HWND g_label_name, g_label_room, g_label_port, g_label_adapter, g_label_host;
static HWND g_label_message, g_label_rooms, g_label_players;
static mn_instance *g_network;
static mn_interface g_interfaces[64];
static size_t g_interface_count;
static mn_room g_rooms[MN_MAX_ROOMS];
static size_t g_room_count;
static char g_last_status[192];
static unsigned g_ui_tick;

static HWND make_control(const char *class_name, const char *text,
                         DWORD style, int id, HWND parent) {
    return CreateWindowExA(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           g_instance_handle, NULL);
}

static void set_font(HWND control) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (control && font) SendMessageA(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static void add_log(const char *message) {
    int length;
    if (!g_log || !message) return;
    length = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)message);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
}

static void add_logf(const char *format, const char *value) {
    char line[320];
    (void)snprintf(line, sizeof(line), format, value ? value : "");
    add_log(line);
}

static void get_text(HWND control, char *buffer, size_t capacity,
                     const char *fallback) {
    if (!buffer || capacity == 0) return;
    buffer[0] = '\0';
    if (control) GetWindowTextA(control, buffer, (int)capacity);
    if (!buffer[0] && fallback) {
        (void)snprintf(buffer, capacity, "%s", fallback);
    }
}

static uint16_t selected_port(void) {
    char text[24];
    char *end = NULL;
    unsigned long value;
    get_text(g_port, text, sizeof(text), "27890");
    value = strtoul(text, &end, 10);
    if (!end || *end != '\0' || value == 0ul || value > 65535ul)
        return MN_DEFAULT_PORT;
    return (uint16_t)value;
}

static const mn_interface *selected_interface(void) {
    LRESULT selection;
    LRESULT data;
    if (!g_interface) return NULL;
    selection = SendMessageA(g_interface, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return NULL;
    data = SendMessageA(g_interface, CB_GETITEMDATA, (WPARAM)selection, 0);
    if (data <= 0 || (size_t)(data - 1) >= g_interface_count) return NULL;
    return &g_interfaces[data - 1];
}

static void refresh_interface_list(void) {
    size_t i;
    char label[256];
    LRESULT item;
    g_interface_count = mn_get_interfaces(g_interfaces, 64);
    if (g_interface_count > 64) g_interface_count = 64;
    SendMessageA(g_interface, CB_RESETCONTENT, 0, 0);
    item = SendMessageA(g_interface, CB_ADDSTRING, 0,
                        (LPARAM)"Automatic (Windows routing)");
    SendMessageA(g_interface, CB_SETITEMDATA, (WPARAM)item, 0);
    for (i = 0; i < g_interface_count; ++i) {
        (void)snprintf(label, sizeof(label), "%s | %s | %s",
                       g_interfaces[i].type == MN_INTERFACE_BLUETOOTH_PAN ? "Bluetooth PAN" :
                       g_interfaces[i].type == MN_INTERFACE_WIFI_DIRECT ? "Wi-Fi Direct" :
                       g_interfaces[i].type == MN_INTERFACE_WIFI ? "Wi-Fi" :
                       g_interfaces[i].type == MN_INTERFACE_ETHERNET ? "Ethernet" :
                       g_interfaces[i].type == MN_INTERFACE_LOOPBACK ? "Loopback" : "Other",
                       g_interfaces[i].description, g_interfaces[i].address);
        item = SendMessageA(g_interface, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageA(g_interface, CB_SETITEMDATA, (WPARAM)item, (LPARAM)(i + 1u));
    }
    SendMessageA(g_interface, CB_SETCURSEL, 0, 0);
}

static void refresh_rooms(void) {
    mn_room current[MN_MAX_ROOMS];
    size_t count = mn_get_rooms(g_network, current, MN_MAX_ROOMS);
    size_t i;
    int changed = count != g_room_count;
    if (count > MN_MAX_ROOMS) count = MN_MAX_ROOMS;
    for (i = 0; !changed && i < count; ++i) {
        if (current[i].room_id != g_rooms[i].room_id ||
            strcmp(current[i].address, g_rooms[i].address) != 0 ||
            current[i].player_count != g_rooms[i].player_count)
            changed = 1;
    }
    if (!changed) return;
    memcpy(g_rooms, current, count * sizeof(mn_room));
    g_room_count = count;
    SendMessageA(g_rooms_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_room_count; ++i) {
        char line[256];
        (void)snprintf(line, sizeof(line), "%s  (%u/%u)  %s:%u",
                       g_rooms[i].name, (unsigned)g_rooms[i].player_count,
                       (unsigned)g_rooms[i].max_players,
                       g_rooms[i].address, (unsigned)g_rooms[i].port);
        SendMessageA(g_rooms_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
}

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

static void update_status(void) {
    mn_info info;
    char line[256];
    mn_get_info(g_network, &info);
    switch (info.status) {
        case MN_STATUS_HOSTING:
            (void)snprintf(line, sizeof(line), "Hosting '%s' | %u/%u players | UDP %u",
                           info.room_name, (unsigned)info.player_count,
                           (unsigned)info.max_players, (unsigned)info.local_port);
            break;
        case MN_STATUS_CONNECTED:
            (void)snprintf(line, sizeof(line), "Connected to '%s' | %u players | ping %.1f ms",
                           info.room_name, (unsigned)info.player_count,
                           info.ping_available ? info.ping_ms : 0.0f);
            break;
        case MN_STATUS_SEARCHING:
            (void)snprintf(line, sizeof(line), "Searching for rooms on UDP %u...",
                           (unsigned)selected_port());
            break;
        case MN_STATUS_CONNECTING:
            (void)snprintf(line, sizeof(line), "Connecting to %s...", info.room_name);
            break;
        default:
            (void)snprintf(line, sizeof(line), "Offline | %s",
                           info.last_error[0] ? info.last_error : "Ready");
            break;
    }
    SetWindowTextA(g_status, line);
    if (strcmp(line, g_last_status) != 0) {
        add_log(line);
        (void)snprintf(g_last_status, sizeof(g_last_status), "%s", line);
    }
}

static void drain_events(void) {
    mn_event event;
    char line[320];
    while (mn_poll_event(g_network, &event)) {
        switch (event.type) {
            case MN_EVENT_CONNECTED:
                add_logf("Connected: %s", event.text);
                refresh_players();
                break;
            case MN_EVENT_DISCONNECTED:
                add_logf("Disconnected: %s", event.text);
                refresh_players();
                break;
            case MN_EVENT_PLAYER_JOINED:
                (void)snprintf(line, sizeof(line), "Player joined: %s (#%u)",
                               event.player.name, (unsigned)event.player_id);
                add_log(line);
                refresh_players();
                break;
            case MN_EVENT_PLAYER_LEFT:
                add_logf("Player left: %s", event.text);
                refresh_players();
                break;
            case MN_EVENT_ROOM_FOUND:
                add_logf("Room found: %s", event.text);
                break;
            case MN_EVENT_MATCH_STARTED:
                add_log("The host started the match.");
                break;
            case MN_EVENT_MESSAGE:
                (void)snprintf(line, sizeof(line), "Message from player #%u: %.*s",
                               (unsigned)event.player_id, (int)event.data_size,
                               (const char *)event.data);
                add_log(line);
                break;
            case MN_EVENT_PAIRING_REQUEST: {
                int answer = MessageBoxA(g_window,
                    "A device is requesting to join. Approve this device?",
                    "MultiNet device pairing", MB_YESNO | MB_ICONQUESTION);
                (void)mn_approve_pairing(g_network, event.device_id,
                                         answer == IDYES ? 1 : 0);
                break;
            }
            case MN_EVENT_PAIRING_REJECTED:
                add_logf("Pairing rejected: %s", event.text);
                break;
            case MN_EVENT_ERROR:
                add_logf("Network error: %s", event.text);
                break;
            default:
                break;
        }
    }
}

static void do_host(void) {
    char name[MN_PLAYER_NAME_LENGTH], room[MN_ROOM_NAME_LENGTH];
    const mn_interface *iface = selected_interface();
    int ok;
    get_text(g_name, name, sizeof(name), "Player");
    get_text(g_room, room, sizeof(room), "MultiNet Room");
    mn_leave(g_network);
    ok = iface ? mn_host_on(g_network, name, room, MN_MAX_PLAYERS, iface->address)
               : mn_host(g_network, name, room, MN_MAX_PLAYERS);
    if (!ok) {
        mn_info info;
        mn_get_info(g_network, &info);
        add_logf("Host failed: %s", info.last_error);
    } else {
        add_log("Room hosted. Clients can search or enter this PC's IP address.");
    }
    refresh_players();
    update_status();
}

static void do_scan(void) {
    const mn_interface *iface = selected_interface();
    char host[64];
    int ok;
    get_text(g_host, host, sizeof(host), "");
    mn_stop_discovery(g_network);
    g_room_count = 0;
    SendMessageA(g_rooms_list, LB_RESETCONTENT, 0, 0);
    if (host[0])
        ok = iface ? mn_discover_at_from(g_network, iface->address, host, selected_port())
                   : mn_discover_at(g_network, host, selected_port());
    else
        ok = iface ? mn_discover_on(g_network, iface) : mn_discover_auto(g_network);
    if (!ok) {
        mn_info info;
        mn_get_info(g_network, &info);
        add_logf("Discovery could not start: %s", info.last_error);
    } else if (host[0]) {
        add_logf("Probing %s directly using the selected adapter (if any)...", host);
    } else {
        add_log(iface ? "Searching on the selected adapter..." :
                        "Searching all active LAN adapters (broadcast, subnet probes and mDNS)...");
    }
    update_status();
}

static void do_join(void) {
    char name[MN_PLAYER_NAME_LENGTH], host[64];
    const mn_interface *iface = selected_interface();
    LRESULT selection = SendMessageA(g_rooms_list, LB_GETCURSEL, 0, 0);
    uint16_t port = selected_port();
    int ok;
    get_text(g_name, name, sizeof(name), "Player");
    if (selection != LB_ERR && (size_t)selection < g_room_count) {
        mn_room room = g_rooms[selection];
        if (iface)
            ok = mn_join_from(g_network, iface->address, room.address,
                              room.port, name);
        else
            ok = mn_join_room(g_network, &room, name);
        (void)snprintf(host, sizeof(host), "%s:%u", room.address, (unsigned)room.port);
    } else {
        get_text(g_host, host, sizeof(host), "127.0.0.1");
        if (iface)
            ok = mn_join_from(g_network, iface->address, host, port, name);
        else
            ok = mn_join(g_network, host, port, name);
    }
    if (!ok) {
        mn_info info;
        mn_get_info(g_network, &info);
        add_logf("Join failed: %s", info.last_error);
    } else {
        add_logf("Joining %s. Keep this window open while it connects.", host);
    }
    update_status();
}

static void do_send(void) {
    char message[MN_MAX_MESSAGE_BYTES + 1u];
    get_text(GetDlgItem(g_window, ID_SEND_TEXT), message,
             sizeof(message), "Hello from MultiNet!");
    if (!mn_is_connected(g_network)) {
        add_log("Join or host a room before sending a message.");
        return;
    }
    if (mn_send_message(g_network, 0, 1, message, strlen(message)))
        add_log("Message sent on channel 1.");
    else
        add_log("Message could not be queued.");
}

static void do_refresh_adapters(void) {
    refresh_interface_list();
    add_log("Network adapters refreshed. Newly connected Bluetooth PAN and Wi-Fi Direct links should now appear.");
}

static void do_ping(void) {
    mn_info info;
    uint32_t target_player_id = 0;
    if (!g_network) return;
    mn_get_info(g_network, &info);
    if (info.role == MN_ROLE_CLIENT && info.status == MN_STATUS_CONNECTED) {
        target_player_id = 0;
    } else if (info.role == MN_ROLE_HOST && info.status == MN_STATUS_HOSTING) {
        mn_player players[MN_MAX_PLAYERS];
        size_t count = mn_get_players(g_network, players, MN_MAX_PLAYERS);
        LRESULT selected = SendMessageA(g_players_list, LB_GETCURSEL, 0, 0);
        size_t i;
        if (selected != LB_ERR && (size_t)selected < count)
            target_player_id = players[selected].id;
        else {
            for (i = 0; i < count; ++i) {
                if (players[i].id != info.local_player_id) {
                    target_player_id = players[i].id;
                    break;
                }
            }
        }
        if (target_player_id == 0 || target_player_id == info.local_player_id) {
            add_log("Select a remote player to ping, or wait for someone to join.");
            return;
        }
    } else {
        add_log("Join or host a room before measuring latency.");
        return;
    }
    if (mn_ping(g_network, target_player_id))
        add_log("Ping request sent. The result will appear in the status line.");
    else
        add_log("Ping could not start. A previous ping may still be pending.");
}

static void create_controls(HWND window) {
    g_label_name = make_control("STATIC", "Player", 0, -1, window);
    g_name = make_control("EDIT", "Player", WS_BORDER | ES_AUTOHSCROLL, ID_NAME, window);
    g_label_room = make_control("STATIC", "Room", 0, -1, window);
    g_room = make_control("EDIT", "MultiNet Room", WS_BORDER | ES_AUTOHSCROLL, ID_ROOM, window);
    g_label_port = make_control("STATIC", "Port", 0, -1, window);
    g_port = make_control("EDIT", "27890", WS_BORDER | ES_AUTOHSCROLL, ID_PORT, window);
    g_label_adapter = make_control("STATIC", "Network adapter", 0, -1, window);
    g_interface = make_control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                               ID_INTERFACE, window);
    g_label_host = make_control("STATIC", "Host IP", 0, -1, window);
    g_host = make_control("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, ID_HOST, window);
    make_control("BUTTON", "Host room", WS_TABSTOP, ID_HOST_BTN, window);
    make_control("BUTTON", "Find rooms", WS_TABSTOP, ID_SCAN_BTN, window);
    make_control("BUTTON", "Join selected / IP", WS_TABSTOP, ID_JOIN_BTN, window);
    make_control("BUTTON", "Leave", WS_TABSTOP, ID_LEAVE_BTN, window);
    make_control("BUTTON", "Start match", WS_TABSTOP, ID_START_BTN, window);
    make_control("BUTTON", "Refresh adapters", WS_TABSTOP, ID_REFRESH_BTN, window);
    make_control("BUTTON", "Ping", WS_TABSTOP, ID_PING_BTN, window);
    g_label_rooms = make_control("STATIC", "Discovered rooms", 0, -1, window);
    g_label_players = make_control("STATIC", "Players in session", 0, -1, window);
    g_rooms_list = make_control("LISTBOX", "", WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
                                ID_ROOMS, window);
    g_players_list = make_control("LISTBOX", "", WS_BORDER | WS_VSCROLL,
                                  ID_PLAYERS, window);
    g_log = make_control("EDIT", "", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL |
                         ES_READONLY | WS_VSCROLL, ID_LOG, window);
    g_label_message = make_control("STATIC", "Message", 0, -1, window);
    make_control("EDIT", "Hello from MultiNet!", WS_BORDER | ES_AUTOHSCROLL,
                 ID_SEND_TEXT, window);
    make_control("BUTTON", "Send", WS_TABSTOP, ID_SEND_BTN, window);
    g_status = make_control("STATIC", "Starting MultiNet...", SS_LEFT, ID_STATUS, window);

    {
        HWND child = GetWindow(window, GW_CHILD);
        while (child) {
            set_font(child);
            child = GetWindow(child, GW_HWNDNEXT);
        }
    }
    refresh_interface_list();
}

static void layout_controls(HWND window) {
    RECT rect;
    int width, height, room_width, player_width, log_top;
    GetClientRect(window, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    room_width = width * 42 / 100;
    player_width = width * 24 / 100;
    SetWindowPos(g_label_name, NULL, 8, 14, 45, 22, SWP_NOZORDER);
    SetWindowPos(g_name, NULL, 53, 10, 130, 25, SWP_NOZORDER);
    SetWindowPos(g_label_room, NULL, 190, 14, 45, 22, SWP_NOZORDER);
    SetWindowPos(g_room, NULL, 238, 10, 170, 25, SWP_NOZORDER);
    SetWindowPos(g_label_port, NULL, 416, 14, 35, 22, SWP_NOZORDER);
    SetWindowPos(g_port, NULL, 452, 10, 62, 25, SWP_NOZORDER);
    SetWindowPos(g_label_adapter, NULL, 525, 14, 95, 22, SWP_NOZORDER);
    SetWindowPos(g_interface, NULL, 620, 10, width - 630, 180, SWP_NOZORDER);
    SetWindowPos(g_label_host, NULL, 8, 51, 52, 22, SWP_NOZORDER);
    SetWindowPos(g_host, NULL, 61, 47, 145, 25, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_HOST_BTN), NULL, 216, 47, 90, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_SCAN_BTN), NULL, 312, 47, 95, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_JOIN_BTN), NULL, 413, 47, 135, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_LEAVE_BTN), NULL, 554, 47, 75, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_START_BTN), NULL, 635, 47, 100, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_REFRESH_BTN), NULL, 741, 47, 105, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_PING_BTN), NULL, 852, 47, 58, 27, SWP_NOZORDER);
    log_top = height - 190;
    SetWindowPos(g_label_rooms, NULL, 8, 80, room_width - 12, 18, SWP_NOZORDER);
    SetWindowPos(g_label_players, NULL, room_width, 80, player_width - 8, 18, SWP_NOZORDER);
    SetWindowPos(g_rooms_list, NULL, 8, 100, room_width - 12, log_top - 108,
                 SWP_NOZORDER);
    SetWindowPos(g_players_list, NULL, room_width, 100, player_width - 8, log_top - 108,
                 SWP_NOZORDER);
    SetWindowPos(g_log, NULL, room_width + player_width, 100,
                 width - room_width - player_width - 8, log_top - 108,
                 SWP_NOZORDER);
    SetWindowPos(g_label_message, NULL, 8, log_top + 8, 55, 22, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_SEND_TEXT), NULL, 65, log_top + 4,
                 width - 132, 25, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_SEND_BTN), NULL, width - 60, log_top + 3,
                 52, 27, SWP_NOZORDER);
    SetWindowPos(g_status, NULL, 8, height - 28, width - 16, 20, SWP_NOZORDER);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            g_window = window;
            create_controls(window);
            SetTimer(window, 1, 33, NULL);
            add_log("MultiNet GUI demo ready.");
            add_log("Connect Ethernet, Wi-Fi, Wi-Fi Direct or Bluetooth PAN in Windows first.");
            add_log("Use Refresh adapters after connecting a new Bluetooth PAN or Wi-Fi Direct link.");
            layout_controls(window);
            return 0;
        case WM_SIZE:
            layout_controls(window);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            limits->ptMinTrackSize.x = 930;
            limits->ptMinTrackSize.y = 560;
            return 0;
        }
        case WM_TIMER:
            if (g_network) {
                mn_update(g_network, 0.033f);
                drain_events();
                refresh_rooms();
                if (++g_ui_tick % 4u == 0u) refresh_players();
                update_status();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case ID_HOST_BTN: do_host(); return 0;
                case ID_SCAN_BTN: do_scan(); return 0;
                case ID_JOIN_BTN: do_join(); return 0;
                case ID_LEAVE_BTN:
                    mn_leave(g_network);
                    add_log("Left the current session.");
                    refresh_players();
                    update_status();
                    return 0;
                case ID_START_BTN:
                    if (!mn_start_match(g_network)) {
                        mn_info info;
                        mn_get_info(g_network, &info);
                        add_logf("Could not start match: %s", info.last_error);
                    }
                    return 0;
                case ID_REFRESH_BTN: do_refresh_adapters(); return 0;
                case ID_PING_BTN: do_ping(); return 0;
                case ID_SEND_BTN: do_send(); return 0;
                case ID_ROOMS:
                    if (HIWORD(wparam) == LBN_SELCHANGE) {
                        LRESULT selection = SendMessageA(g_rooms_list, LB_GETCURSEL, 0, 0);
                        if (selection != LB_ERR && (size_t)selection < g_room_count) {
                            SetWindowTextA(g_host, g_rooms[selection].address);
                            (void)snprintf(g_last_status, sizeof(g_last_status),
                                           "Selected %s", g_rooms[selection].name);
                        }
                    }
                    return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(window, 1);
            if (g_network) {
                mn_leave(g_network);
                mn_destroy(g_network);
                g_network = NULL;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous,
                   LPSTR command_line, int show_command) {
    WNDCLASSEXA window_class;
    HWND window;
    MSG message;
    mn_config config;
    (void)previous;
    (void)command_line;

    g_instance_handle = instance;
    mn_config_init(&config);
    g_network = mn_create(&config);
    if (!g_network) {
        MessageBoxA(NULL, "MultiNet could not initialise its network socket.",
                    "MultiNet", MB_OK | MB_ICONERROR);
        return 1;
    }

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = "MultiNetGuiDemoWindow";
    if (!RegisterClassExA(&window_class)) {
        mn_destroy(g_network);
        MessageBoxA(NULL, "Could not register the MultiNet window class.",
                    "MultiNet", MB_OK | MB_ICONERROR);
        return 1;
    }

    window = CreateWindowExA(0, window_class.lpszClassName,
        "MultiNet LAN Multiplayer Demo", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760,
        NULL, NULL, instance, NULL);
    if (!window) {
        mn_destroy(g_network);
        MessageBoxA(NULL, "Could not create the MultiNet demo window.",
                    "MultiNet", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return (int)message.wParam;
}
