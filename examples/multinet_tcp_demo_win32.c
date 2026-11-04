/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
/* Double-clickable direct TCP test GUI using the framed message API. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "multinet_tcp.h"
#include "multinet_stream.h"

#define ID_HOST_ADDR 101
#define ID_PORT 102
#define ID_HOST 103
#define ID_JOIN 104
#define ID_LEAVE 105
#define ID_LOG 106
#define ID_MESSAGE 107
#define ID_SEND 108
#define ID_STATUS 109

static HINSTANCE g_instance;
static HWND g_window;
static HWND g_host_address;
static HWND g_port;
static HWND g_label_address;
static HWND g_label_port;
static HWND g_log;
static HWND g_message;
static HWND g_status;
static HWND g_host_button;
static HWND g_join_button;
static mn_tcp *g_tcp;
static mn_stream *g_stream;
static int g_stream_error_logged;
static int g_host_mode;
static int g_waiting_reconnect;
static char g_last_status[256];

static HWND make_control(const char *class_name, const char *caption,
                         DWORD style, int id, HWND parent) {
    return CreateWindowExA(0, class_name, caption,
        WS_CHILD | WS_VISIBLE | style,
        0, 0, 100, 24, parent, (HMENU)(INT_PTR)id, g_instance, NULL);
}

static void set_font(HWND control) {
    SendMessageA(control, WM_SETFONT,
                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static void add_log(const char *message) {
    int length;
    if (!g_log || !message) return;
    length = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)message);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
}

static void get_text(HWND control, char *buffer, size_t capacity,
                     const char *fallback) {
    if (!buffer || capacity == 0) return;
    buffer[0] = '\0';
    if (control) GetWindowTextA(control, buffer, (int)capacity);
    if (!buffer[0] && fallback)
        (void)snprintf(buffer, capacity, "%s", fallback);
}

static int read_port(uint16_t *out_port) {
    char text[32];
    char *end = NULL;
    unsigned long value;
    get_text(g_port, text, sizeof(text), "27891");
    value = strtoul(text, &end, 10);
    if (!text[0] || !end || *end != '\0' || value == 0ul ||
        value > 65535ul) return 0;
    *out_port = (uint16_t)value;
    return 1;
}

static int reset_stream(void) {
    if (!g_stream) return 0;
    mn_stream_reset(g_stream);
    g_stream_error_logged = 0;
    return 1;
}

static void update_status(void) {
    mn_tcp_info info;
    char line[256];
    if (!g_tcp) return;
    mn_tcp_get_info(g_tcp, &info);
    switch (info.state) {
        case MN_TCP_LISTENING:
            (void)snprintf(line, sizeof(line),
                "Listening for one TCP peer on port %u",
                (unsigned)info.local_port);
            break;
        case MN_TCP_CONNECTING:
            (void)snprintf(line, sizeof(line),
                "Connecting to %s:%u...",
                info.remote_address, (unsigned)info.local_port);
            break;
        case MN_TCP_CONNECTED:
            (void)snprintf(line, sizeof(line),
                "Connected to %s | local TCP port %u | unencrypted",
                info.remote_address, (unsigned)info.local_port);
            break;
        default:
            (void)snprintf(line, sizeof(line), "Offline | %s",
                info.last_error[0] ? info.last_error : "Host or join a TCP peer.");
            break;
    }
    SetWindowTextA(g_status, line);
    if (strcmp(line, g_last_status) != 0) {
        add_log(line);
        (void)snprintf(g_last_status, sizeof(g_last_status), "%s", line);
    }
}

static void do_host(void) {
    uint16_t port;
    if (!read_port(&port)) {
        add_log("Enter a valid TCP port (1-65535).");
        return;
    }
    if (!mn_tcp_host(g_tcp, NULL, port)) {
        mn_tcp_info info;
        mn_tcp_get_info(g_tcp, &info);
        add_log(info.last_error);
        update_status();
        return;
    }
    g_host_mode = 1;
    g_waiting_reconnect = 0;
    (void)reset_stream();
    add_log("TCP listener started on all local IPv4 interfaces.");
    add_log("The peer must enter this PC's LAN address and the same TCP port.");
    add_log("Direct TCP is not encrypted. Only use it on a trusted network.");
    update_status();
}

static void do_join(void) {
    char host[128];
    uint16_t port;
    get_text(g_host_address, host, sizeof(host), "127.0.0.1");
    if (!read_port(&port)) {
        add_log("Enter a valid TCP port (1-65535).");
        return;
    }
    if (!mn_tcp_join(g_tcp, host, port)) {
        mn_tcp_info info;
        mn_tcp_get_info(g_tcp, &info);
        add_log(info.last_error);
        update_status();
        return;
    }
    g_host_mode = 0;
    g_waiting_reconnect = 0;
    (void)reset_stream();
    add_log("TCP connect started. Keep this window open while it connects.");
    update_status();
}

static void do_leave(void) {
    mn_tcp_close(g_tcp);
    (void)reset_stream();
    g_host_mode = 0;
    g_waiting_reconnect = 0;
    add_log("TCP connection/listener closed.");
    update_status();
}

static void do_send(void) {
    char message[4096];
    get_text(g_message, message, sizeof(message), "");
    if (!message[0]) return;
    if (!g_stream) {
        add_log("The framed message layer is unavailable.");
        return;
    }
    if (!mn_stream_send(g_stream, 1u, message, strlen(message))) {
        add_log("Could not queue the message. Check the connection and queue capacity.");
        return;
    }
    SetWindowTextA(g_message, "");
}

static void poll_messages(void) {
    char buffer[4097];
    size_t received;
    uint16_t channel;
    int result;
    for (;;) {
        result = mn_stream_poll(g_stream, &channel, buffer,
                                sizeof(buffer) - 1u, &received);
        if (result == 0) break;
        if (result < 0) {
            add_log("Incoming message is larger than the GUI buffer.");
            break;
        }
        buffer[received] = '\0';
        if (channel == 1u) add_log(buffer);
        else {
            char line[128];
            (void)snprintf(line, sizeof(line),
                           "Message received on application channel %u.",
                           (unsigned)channel);
            add_log(line);
        }
    }
}

static void create_controls(HWND window) {
    g_label_address = make_control("STATIC", "Peer IPv4 / hostname", 0, -1, window);
    g_host_address = make_control("EDIT", "127.0.0.1",
        WS_BORDER | ES_AUTOHSCROLL, ID_HOST_ADDR, window);
    g_label_port = make_control("STATIC", "TCP port", 0, -1, window);
    g_port = make_control("EDIT", "27891",
        WS_BORDER | ES_AUTOHSCROLL, ID_PORT, window);
    g_host_button = make_control("BUTTON", "Host", WS_TABSTOP, ID_HOST, window);
    g_join_button = make_control("BUTTON", "Connect", WS_TABSTOP, ID_JOIN, window);
    make_control("BUTTON", "Leave", WS_TABSTOP, ID_LEAVE, window);
    g_log = make_control("EDIT", "",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        ID_LOG, window);
    g_message = make_control("EDIT", "",
        WS_BORDER | ES_AUTOHSCROLL, ID_MESSAGE, window);
    make_control("BUTTON", "Send", WS_TABSTOP, ID_SEND, window);
    g_status = make_control("STATIC", "Starting TCP test...",
                            SS_LEFT, ID_STATUS, window);
    {
        HWND child = GetWindow(window, GW_CHILD);
        while (child) {
            set_font(child);
            child = GetWindow(child, GW_HWNDNEXT);
        }
    }
}

static void layout_controls(HWND window) {
    RECT rect;
    int width, height;
    GetClientRect(window, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    SetWindowPos(g_label_address, NULL, 10, 16, 140, 22, SWP_NOZORDER);
    SetWindowPos(g_host_address, NULL, 150, 12, 240, 27, SWP_NOZORDER);
    SetWindowPos(g_label_port, NULL, 400, 16, 55, 22, SWP_NOZORDER);
    SetWindowPos(g_port, NULL, 455, 12, 85, 27, SWP_NOZORDER);
    SetWindowPos(g_host_button, NULL, 550, 12, 70, 28, SWP_NOZORDER);
    SetWindowPos(g_join_button, NULL, 626, 12, 85, 28, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_LEAVE), NULL, 717, 12, 70, 28, SWP_NOZORDER);
    SetWindowPos(g_log, NULL, 10, 55, width - 20, height - 145, SWP_NOZORDER);
    SetWindowPos(g_message, NULL, 10, height - 78,
                 width - 100, 28, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_SEND), NULL, width - 82,
                 height - 79, 72, 30, SWP_NOZORDER);
    SetWindowPos(g_status, NULL, 10, height - 34,
                 width - 20, 22, SWP_NOZORDER);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            mn_stream_transport transport;
            g_window = window;
            g_tcp = mn_tcp_create();
            if (!g_tcp) {
                MessageBoxA(window, "Could not initialise TCP sockets.",
                            "MultiNet TCP", MB_OK | MB_ICONERROR);
                return -1;
            }
            create_controls(window);
            if (!mn_stream_transport_from_tcp(g_tcp, &transport) ||
                !(g_stream = mn_stream_create(&transport, 4096u))) {
                mn_tcp_destroy(g_tcp);
                g_tcp = NULL;
                MessageBoxA(window, "Could not initialise framed TCP messaging.",
                            "MultiNet TCP", MB_OK | MB_ICONERROR);
                return -1;
            }
            SetTimer(window, 1, 33, NULL);
            add_log("MultiNet direct TCP test ready.");
            add_log("Host or connect using an address and permitted TCP port.");
            add_log("Messages use the shared framed-stream API.");
            layout_controls(window);
            return 0;
        }
        case WM_SIZE:
            layout_controls(window);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            limits->ptMinTrackSize.x = 820;
            limits->ptMinTrackSize.y = 520;
            return 0;
        }
        case WM_TIMER:
            if (g_tcp && g_stream) {
                if (!mn_stream_update(g_stream)) {
                    mn_tcp_info info;
                    if (!g_stream_error_logged) {
                        add_log(mn_stream_last_error(g_stream));
                        g_stream_error_logged = 1;
                    }
                    mn_tcp_get_info(g_tcp, &info);
                    if (g_host_mode && info.state == MN_TCP_LISTENING) {
                        g_waiting_reconnect = 1;
                        (void)mn_tcp_update(g_tcp);
                        mn_tcp_get_info(g_tcp, &info);
                        if (g_waiting_reconnect && info.state == MN_TCP_CONNECTED) {
                            mn_stream_reset(g_stream);
                            g_stream_error_logged = 0;
                            g_waiting_reconnect = 0;
                            add_log("Replacement TCP client connected. New framed session started.");
                        }
                    }
                }
                poll_messages();
                update_status();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case ID_HOST: do_host(); return 0;
                case ID_JOIN: do_join(); return 0;
                case ID_LEAVE: do_leave(); return 0;
                case ID_SEND: do_send(); return 0;
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            if (g_stream) {
                mn_stream_destroy(g_stream);
                g_stream = NULL;
            }
            if (g_tcp) {
                mn_tcp_destroy(g_tcp);
                g_tcp = NULL;
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
    (void)previous;
    (void)command_line;
    g_instance = instance;
    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = "MultiNetTcpDemoWindow";
    if (!RegisterClassExA(&window_class)) {
        MessageBoxA(NULL, "Could not register the TCP demo window.",
                    "MultiNet TCP", MB_OK | MB_ICONERROR);
        return 1;
    }
    window = CreateWindowExA(0, window_class.lpszClassName,
        "MultiNet Direct TCP Test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 920, 650,
        NULL, NULL, instance, NULL);
    if (!window) {
        MessageBoxA(NULL, "Could not create the TCP demo window.",
                    "MultiNet TCP", MB_OK | MB_ICONERROR);
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
