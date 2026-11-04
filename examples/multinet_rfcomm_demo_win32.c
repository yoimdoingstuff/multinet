/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
/* Double-clickable Windows RFCOMM transport test. This tests the raw stream
   backend only; it is not yet a MultiNet UDP room client. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "multinet_rfcomm.h"
#include "multinet_stream.h"

#define ID_NAME       101
#define ID_HOST       102
#define ID_SCAN       103
#define ID_JOIN       104
#define ID_DISCONNECT 105
#define ID_SECURE     106
#define ID_DEVICES    107
#define ID_LOG        108
#define ID_MESSAGE    109
#define ID_SEND       110
#define ID_STATUS     111
#define WM_SCAN_DONE  (WM_APP + 10)

static HINSTANCE g_instance;
static HWND g_window;
static HWND g_name;
static HWND g_host;
static HWND g_scan;
static HWND g_join;
static HWND g_disconnect;
static HWND g_secure;
static HWND g_devices_list;
static HWND g_log;
static HWND g_message;
static HWND g_status;
static mn_rfcomm *g_connection;
static mn_stream *g_stream;
static int g_stream_error_logged;
static int g_host_waiting_reconnect;
static mn_rfcomm_device g_devices[MN_RFCOMM_MAX_DEVICES];
static size_t g_device_count;
static char g_scan_error[160];
static HANDLE g_scan_thread;
static volatile LONG g_scan_running;
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

static int secure_link_required(void) {
    return SendMessageA(g_secure, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static int reset_framed_stream(void) {
    mn_stream_transport transport;
    if (g_stream) {
        mn_stream_destroy(g_stream);
        g_stream = NULL;
    }
    if (!g_connection ||
        !mn_stream_transport_from_rfcomm(g_connection, &transport))
        return 0;
    g_stream = mn_stream_create(&transport, 4096u);
    g_stream_error_logged = 0;
    g_host_waiting_reconnect = 0;
    return g_stream != NULL;
}

static void update_status(void) {
    mn_rfcomm_info info;
    char line[256];
    if (!g_connection) return;
    mn_rfcomm_get_info(g_connection, &info);
    switch (info.state) {
        case MN_RFCOMM_LISTENING:
            (void)snprintf(line, sizeof(line),
                "Advertising '%s' on RFCOMM channel %u. Waiting for a Bluetooth peer.",
                info.service_name, (unsigned)info.local_channel);
            break;
        case MN_RFCOMM_CONNECTING:
            (void)snprintf(line, sizeof(line),
                "Connecting to Bluetooth device %012llX...",
                (unsigned long long)info.remote_address);
            break;
        case MN_RFCOMM_CONNECTED:
            (void)snprintf(line, sizeof(line),
                "Connected over Bluetooth RFCOMM to %012llX.",
                (unsigned long long)info.remote_address);
            break;
        default:
            (void)snprintf(line, sizeof(line), "Idle. %s",
                info.last_error[0] ? info.last_error :
                "Host a service or scan for a nearby device.");
            break;
    }
    SetWindowTextA(g_status, line);
    if (strcmp(line, g_last_status) != 0) {
        add_log(line);
        (void)snprintf(g_last_status, sizeof(g_last_status), "%s", line);
    }
}

static DWORD WINAPI scan_thread_proc(LPVOID parameter) {
    mn_rfcomm *scanner;
    mn_rfcomm_info info;
    (void)parameter;
    g_device_count = 0;
    g_scan_error[0] = '\0';
    scanner = mn_rfcomm_create();
    if (!scanner) {
        (void)snprintf(g_scan_error, sizeof(g_scan_error),
                       "Could not initialise Windows Bluetooth sockets.");
    } else {
        g_device_count = mn_rfcomm_discover(scanner, g_devices,
                                            MN_RFCOMM_MAX_DEVICES);
        mn_rfcomm_get_info(scanner, &info);
        (void)snprintf(g_scan_error, sizeof(g_scan_error), "%s",
                       info.last_error);
        mn_rfcomm_destroy(scanner);
    }
    InterlockedExchange(&g_scan_running, 0);
    if (g_window) PostMessageA(g_window, WM_SCAN_DONE, 0, 0);
    return 0;
}

static void do_host(void) {
    char name[MN_RFCOMM_NAME_LENGTH];
    mn_rfcomm_info info;
    get_text(g_name, name, sizeof(name), "Player");
    if (!mn_rfcomm_host(g_connection, name, secure_link_required())) {
        mn_rfcomm_get_info(g_connection, &info);
        add_log(info.last_error[0] ? info.last_error :
                "Could not advertise the Bluetooth RFCOMM service.");
        update_status();
        return;
    }
    mn_stream_reset(g_stream);
    g_stream_error_logged = 0;
    g_host_waiting_reconnect = 0;
    add_log("Service is discoverable by another MultiNet RFCOMM demo.");
    add_log("On the other PC, scan for services and connect to this device.");
    add_log("Windows may display its own Bluetooth pairing/passkey prompt.");
    update_status();
}

static void do_scan(void) {
    DWORD thread_id;
    if (InterlockedCompareExchange(&g_scan_running, 1, 0) != 0) {
        add_log("A Bluetooth service scan is already running.");
        return;
    }
    if (g_scan_thread) {
        WaitForSingleObject(g_scan_thread, 0);
        CloseHandle(g_scan_thread);
        g_scan_thread = NULL;
    }
    EnableWindow(g_scan, FALSE);
    add_log("Scanning visible Bluetooth devices for the MultiNet service.");
    add_log("Windows service discovery can take several seconds.");
    g_scan_thread = CreateThread(NULL, 0, scan_thread_proc, NULL, 0, &thread_id);
    if (!g_scan_thread) {
        InterlockedExchange(&g_scan_running, 0);
        EnableWindow(g_scan, TRUE);
        add_log("Could not start the Bluetooth discovery thread.");
    }
}

static void finish_scan(void) {
    size_t i;
    char line[256];
    if (g_scan_thread) {
        WaitForSingleObject(g_scan_thread, INFINITE);
        CloseHandle(g_scan_thread);
        g_scan_thread = NULL;
    }
    EnableWindow(g_scan, TRUE);
    if (g_device_count > MN_RFCOMM_MAX_DEVICES)
        g_device_count = MN_RFCOMM_MAX_DEVICES;
    SendMessageA(g_devices_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_device_count; ++i) {
        LRESULT item;
        (void)snprintf(line, sizeof(line), "%s  [%012llX]  channel %u",
            g_devices[i].name,
            (unsigned long long)g_devices[i].address,
            (unsigned)g_devices[i].channel);
        item = SendMessageA(g_devices_list, LB_ADDSTRING, 0, (LPARAM)line);
        SendMessageA(g_devices_list, LB_SETITEMDATA, (WPARAM)item,
                     (LPARAM)(i + 1u));
    }
    if (g_device_count == 0) {
        add_log(g_scan_error[0] ? g_scan_error :
                "No MultiNet RFCOMM service was found on visible devices.");
    } else {
        (void)snprintf(line, sizeof(line), "Found %u MultiNet Bluetooth service(s).",
                       (unsigned)g_device_count);
        add_log(line);
    }
}

static void do_join(void) {
    LRESULT selection = SendMessageA(g_devices_list, LB_GETCURSEL, 0, 0);
    LRESULT data;
    size_t index;
    mn_rfcomm_info info;
    if (selection == LB_ERR) {
        add_log("Select a discovered Bluetooth service first.");
        return;
    }
    data = SendMessageA(g_devices_list, LB_GETITEMDATA,
                        (WPARAM)selection, 0);
    if (data <= 0) {
        add_log("The selected Bluetooth service entry is invalid.");
        return;
    }
    index = (size_t)(data - 1);
    if (index >= g_device_count) {
        add_log("The selected Bluetooth service is no longer available.");
        return;
    }
    if (!mn_rfcomm_join(g_connection, &g_devices[index],
                        secure_link_required())) {
        mn_rfcomm_get_info(g_connection, &info);
        add_log(info.last_error[0] ? info.last_error :
                "Could not start the Bluetooth connection.");
        update_status();
        return;
    }
    mn_stream_reset(g_stream);
    g_stream_error_logged = 0;
    g_host_waiting_reconnect = 0;
    add_log("Connection started. Keep this window open while Windows connects.");
    update_status();
}

static void do_send(void) {
    char text[1500];
    size_t length;
    mn_rfcomm_info info;
    get_text(g_message, text, sizeof(text), "");
    if (!text[0]) return;
    if (!g_stream) {
        add_log("The framed message channel is not available.");
        return;
    }
    mn_rfcomm_get_info(g_connection, &info);
    if (info.state != MN_RFCOMM_CONNECTED) {
        add_log("Connect to a Bluetooth peer before sending.");
        return;
    }
    length = strlen(text);
    if (!mn_stream_send(g_stream, 1u, text, length)) {
        add_log("The framed stream send queue is full or the message is too large.");
        return;
    }
    SetWindowTextA(g_message, "");
}

static void poll_incoming(void) {
    char buffer[4097];
    size_t received;
    uint16_t channel;
    int result;
    if (!g_stream) return;
    for (;;) {
        result = mn_stream_poll(g_stream, &channel, buffer,
                                sizeof(buffer) - 1u, &received);
        if (result == 0) break;
        if (result < 0) {
            add_log("Incoming framed message exceeded the GUI buffer.");
            break;
        }
        buffer[received] = '\0';
        if (channel == 1u) add_log(buffer);
        else {
            char line[128];
            (void)snprintf(line, sizeof(line),
                           "Received message on application channel %u.",
                           (unsigned)channel);
            add_log(line);
        }
    }
}

static void create_controls(HWND window) {
    g_name = make_control("EDIT", "Player", WS_BORDER | ES_AUTOHSCROLL,
                          ID_NAME, window);
    g_host = make_control("BUTTON", "Advertise service",
                          WS_TABSTOP, ID_HOST, window);
    g_scan = make_control("BUTTON", "Find nearby services",
                          WS_TABSTOP, ID_SCAN, window);
    g_join = make_control("BUTTON", "Connect selected",
                          WS_TABSTOP, ID_JOIN, window);
    g_disconnect = make_control("BUTTON", "Disconnect",
                                WS_TABSTOP, ID_DISCONNECT, window);
    g_secure = make_control("BUTTON", "Require Bluetooth pairing + encryption",
                            BS_AUTOCHECKBOX | WS_TABSTOP, ID_SECURE, window);
    g_devices_list = make_control("LISTBOX", "",
        WS_BORDER | LBS_NOTIFY | WS_VSCROLL, ID_DEVICES, window);
    g_log = make_control("EDIT", "",
        WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        ID_LOG, window);
    g_message = make_control("EDIT", "", WS_BORDER | ES_AUTOHSCROLL,
                             ID_MESSAGE, window);
    make_control("BUTTON", "Send", WS_TABSTOP, ID_SEND, window);
    g_status = make_control("STATIC", "Starting Bluetooth RFCOMM test...",
                            SS_LEFT, ID_STATUS, window);
    SendMessageA(g_secure, BM_SETCHECK, BST_CHECKED, 0);

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
    int width, height, left_width, log_top;
    GetClientRect(window, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    left_width = width * 42 / 100;
    log_top = height - 150;
    SetWindowPos(g_name, NULL, 12, 12, 180, 26, SWP_NOZORDER);
    SetWindowPos(g_host, NULL, 202, 11, 145, 28, SWP_NOZORDER);
    SetWindowPos(g_secure, NULL, 360, 14, width - 372, 24, SWP_NOZORDER);
    SetWindowPos(g_scan, NULL, 12, 52, 155, 28, SWP_NOZORDER);
    SetWindowPos(g_join, NULL, 175, 52, 135, 28, SWP_NOZORDER);
    SetWindowPos(g_disconnect, NULL, 318, 52, 100, 28, SWP_NOZORDER);
    SetWindowPos(g_devices_list, NULL, 12, 92, left_width - 18,
                 log_top - 98, SWP_NOZORDER);
    SetWindowPos(g_log, NULL, left_width, 92, width - left_width - 12,
                 log_top - 98, SWP_NOZORDER);
    SetWindowPos(g_message, NULL, 12, log_top + 12,
                 width - 100, 27, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, ID_SEND), NULL, width - 78,
                 log_top + 11, 66, 29, SWP_NOZORDER);
    SetWindowPos(g_status, NULL, 12, height - 28, width - 24, 20, SWP_NOZORDER);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            g_window = window;
            g_connection = mn_rfcomm_create();
            if (!g_connection) {
                MessageBoxA(window,
                    "Could not initialise Windows Bluetooth sockets.",
                    "MultiNet RFCOMM", MB_OK | MB_ICONERROR);
                return -1;
            }
            create_controls(window);
            if (!reset_framed_stream()) {
                mn_rfcomm_destroy(g_connection);
                g_connection = NULL;
                MessageBoxA(window,
                    "Could not initialise the framed stream adapter.",
                    "MultiNet RFCOMM", MB_OK | MB_ICONERROR);
                return -1;
            }
            SetTimer(window, 1, 33, NULL);
            add_log("MultiNet native Bluetooth RFCOMM test ready.");
            add_log("Pair devices in Windows if prompted. No radio is enabled automatically.");
            add_log("This tests a one-peer byte stream, not the UDP 16-player game session.");
            layout_controls(window);
            return 0;
        case WM_SIZE:
            layout_controls(window);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            limits->ptMinTrackSize.x = 820;
            limits->ptMinTrackSize.y = 540;
            return 0;
        }
        case WM_TIMER:
            if (g_connection) {
                if (!mn_stream_update(g_stream)) {
                    mn_rfcomm_info transport_info;
                    if (!g_stream_error_logged) {
                        add_log(mn_stream_last_error(g_stream));
                        g_stream_error_logged = 1;
                    }
                    /* A failed framed stream no longer pumps the transport.
                       Keep a listening RFCOMM host alive so a new peer can be
                       accepted, then discard old framed state on reconnect. */
                    /* Check the state immediately after the stream noticed
                       EOF, before accept() can replace the old peer. */
                    mn_rfcomm_get_info(g_connection, &transport_info);
                    if (transport_info.state == MN_RFCOMM_LISTENING)
                        g_host_waiting_reconnect = 1;
                    (void)mn_rfcomm_update(g_connection);
                    mn_rfcomm_get_info(g_connection, &transport_info);
                    if (g_host_waiting_reconnect &&
                        transport_info.state == MN_RFCOMM_CONNECTED) {
                        mn_stream_reset(g_stream);
                        g_stream_error_logged = 0;
                        g_host_waiting_reconnect = 0;
                        add_log("A Bluetooth peer reconnected. Old queued messages were discarded.");
                    }
                }
                poll_incoming();
                update_status();
            }
            return 0;
        case WM_SCAN_DONE:
            finish_scan();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case ID_HOST:
                    do_host();
                    return 0;
                case ID_SCAN:
                    do_scan();
                    return 0;
                case ID_JOIN:
                    do_join();
                    return 0;
                case ID_DISCONNECT:
                    mn_rfcomm_close(g_connection);
                    (void)reset_framed_stream();
                    add_log("Bluetooth connection closed. The stream adapter is ready for another attempt.");
                    update_status();
                    return 0;
                case ID_SEND:
                    do_send();
                    return 0;
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            if (g_scan_thread) {
                WaitForSingleObject(g_scan_thread, INFINITE);
                CloseHandle(g_scan_thread);
                g_scan_thread = NULL;
            }
            if (g_stream) {
                mn_stream_destroy(g_stream);
                g_stream = NULL;
            }
            if (g_connection) {
                mn_rfcomm_destroy(g_connection);
                g_connection = NULL;
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
    window_class.lpszClassName = "MultiNetRfcommDemoWindow";
    if (!RegisterClassExA(&window_class)) {
        MessageBoxA(NULL, "Could not register the RFCOMM test window.",
                    "MultiNet RFCOMM", MB_OK | MB_ICONERROR);
        return 1;
    }
    window = CreateWindowExA(0, window_class.lpszClassName,
        "MultiNet Bluetooth RFCOMM Test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1080, 720,
        NULL, NULL, instance, NULL);
    if (!window) {
        MessageBoxA(NULL, "Could not create the RFCOMM test window.",
                    "MultiNet RFCOMM", MB_OK | MB_ICONERROR);
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
