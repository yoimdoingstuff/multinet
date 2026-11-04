/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
/* Optional native Bluetooth RFCOMM stream transport.
   This is deliberately separate from the UDP mn_instance room protocol:
   RFCOMM is a byte stream and needs explicit application-level framing. */
#include "multinet_rfcomm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ws2bth.h>
#include <limits.h>

#ifndef SOL_RFCOMM
#define SOL_RFCOMM 3
#endif

static const GUID MN_RFCOMM_SERVICE_UUID =
    {0x4d4e5432, 0x6f6a, 0x4d4e, {0x91, 0x22, 0x4d, 0x4e, 0x52, 0x46, 0x43, 0x4d}};

struct mn_rfcomm {
    SOCKET listener;
    SOCKET socket;
    int winsock_started;
    int service_registered;
    SOCKADDR_BTH local_address;
    WCHAR service_name_w[MN_RFCOMM_NAME_LENGTH];
    mn_rfcomm_info info;
};

static void mn_rfcomm_set_error(mn_rfcomm *connection, const char *message) {
    if (!connection) return;
    (void)snprintf(connection->info.last_error,
                   sizeof(connection->info.last_error), "%s",
                   message ? message : "Bluetooth RFCOMM error");
}

static void mn_rfcomm_set_wsa_error(mn_rfcomm *connection,
                                    const char *operation, int error_code) {
    char message[160];
    (void)snprintf(message, sizeof(message), "%s failed (Winsock error %d)",
                   operation, error_code);
    mn_rfcomm_set_error(connection, message);
}

static int mn_rfcomm_nonblocking(SOCKET socket_value) {
    u_long enabled = 1;
    return ioctlsocket(socket_value, FIONBIO, &enabled) == 0;
}

static int mn_rfcomm_set_security(SOCKET socket_value, int secure_link) {
    ULONG enabled = 1;
    if (!secure_link) return 1;
    if (setsockopt(socket_value, SOL_RFCOMM, SO_BTH_AUTHENTICATE,
                   (const char *)&enabled, (int)sizeof(enabled)) == SOCKET_ERROR)
        return 0;
    if (setsockopt(socket_value, SOL_RFCOMM, SO_BTH_ENCRYPT,
                   (const char *)&enabled, (int)sizeof(enabled)) == SOCKET_ERROR)
        return 0;
    return 1;
}

static void mn_rfcomm_close_socket(SOCKET *socket_value) {
    if (socket_value && *socket_value != INVALID_SOCKET) {
        closesocket(*socket_value);
        *socket_value = INVALID_SOCKET;
    }
}

static void mn_rfcomm_abort(mn_rfcomm *connection) {
    char saved_error[160];
    if (!connection) return;
    (void)snprintf(saved_error, sizeof(saved_error), "%s",
                   connection->info.last_error);
    mn_rfcomm_close(connection);
    if (saved_error[0]) mn_rfcomm_set_error(connection, saved_error);
}

static void mn_rfcomm_unregister_service(mn_rfcomm *connection) {
    WSAQUERYSETW query;
    CSADDR_INFO address_info;
    if (!connection || !connection->service_registered) return;
    ZeroMemory(&query, sizeof(query));
    ZeroMemory(&address_info, sizeof(address_info));
    query.dwSize = sizeof(query);
    query.lpszServiceInstanceName = connection->service_name_w;
    query.lpServiceClassId = (LPGUID)&MN_RFCOMM_SERVICE_UUID;
    query.dwNameSpace = NS_BTH;
    query.dwNumberOfCsAddrs = 1;
    query.lpcsaBuffer = &address_info;
    address_info.LocalAddr.lpSockaddr = (LPSOCKADDR)&connection->local_address;
    address_info.LocalAddr.iSockaddrLength = sizeof(connection->local_address);
    address_info.iSocketType = SOCK_STREAM;
    address_info.iProtocol = BTHPROTO_RFCOMM;
    (void)WSASetServiceW(&query, RNRSERVICE_DELETE, 0);
    connection->service_registered = 0;
}

static void mn_rfcomm_drop_peer(mn_rfcomm *connection) {
    if (!connection) return;
    mn_rfcomm_close_socket(&connection->socket);
    connection->info.remote_address = 0;
    connection->info.state = connection->listener != INVALID_SOCKET
        ? MN_RFCOMM_LISTENING : MN_RFCOMM_IDLE;
}

static int mn_rfcomm_register_service(mn_rfcomm *connection) {
    WSAQUERYSETW query;
    CSADDR_INFO address_info;
    ZeroMemory(&query, sizeof(query));
    ZeroMemory(&address_info, sizeof(address_info));
    query.dwSize = sizeof(query);
    query.lpszServiceInstanceName = connection->service_name_w;
    query.lpServiceClassId = (LPGUID)&MN_RFCOMM_SERVICE_UUID;
    query.dwNameSpace = NS_BTH;
    query.dwNumberOfCsAddrs = 1;
    query.lpcsaBuffer = &address_info;
    address_info.LocalAddr.lpSockaddr = (LPSOCKADDR)&connection->local_address;
    address_info.LocalAddr.iSockaddrLength = sizeof(connection->local_address);
    address_info.iSocketType = SOCK_STREAM;
    address_info.iProtocol = BTHPROTO_RFCOMM;
    if (WSASetServiceW(&query, RNRSERVICE_REGISTER, 0) == SOCKET_ERROR) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth SDP service registration",
                                WSAGetLastError());
        return 0;
    }
    connection->service_registered = 1;
    return 1;
}

static int mn_rfcomm_to_utf8(char *out, size_t capacity, const WCHAR *text) {
    int converted;
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    if (!text || !text[0]) return 1;
    if (capacity > (size_t)INT_MAX) return 0;
    converted = WideCharToMultiByte(CP_UTF8, 0, text, -1, out,
                                    (int)capacity, NULL, NULL);
    if (converted == 0) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static int mn_rfcomm_query_device(mn_rfcomm *connection, uint64_t address,
                                  const char *device_name,
                                  mn_rfcomm_device *out_device) {
    SOCKADDR_BTH peer;
    WCHAR context[128];
    DWORD context_length = (DWORD)(sizeof(context) / sizeof(context[0]));
    WSAQUERYSETW restrictions;
    HANDLE lookup = NULL;
    DWORD flags = LUP_FLUSHCACHE;
    int found = 0;
    ZeroMemory(&peer, sizeof(peer));
    peer.addressFamily = AF_BTH;
    peer.btAddr = (BTH_ADDR)address;
    peer.port = 0;
    if (WSAAddressToStringW((LPSOCKADDR)&peer, sizeof(peer), NULL,
                            context, &context_length) == SOCKET_ERROR)
        return 0;

    ZeroMemory(&restrictions, sizeof(restrictions));
    restrictions.dwSize = sizeof(restrictions);
    restrictions.lpServiceClassId = (LPGUID)&MN_RFCOMM_SERVICE_UUID;
    restrictions.dwNameSpace = NS_BTH;
    restrictions.lpszContext = context;
    if (WSALookupServiceBeginW(&restrictions, flags, &lookup) == SOCKET_ERROR) {
        int error_code = WSAGetLastError();
        if (error_code != WSASERVICE_NOT_FOUND && error_code != WSA_E_NO_MORE)
            mn_rfcomm_set_wsa_error(connection, "Bluetooth SDP query", error_code);
        return 0;
    }

    for (;;) {
        union {
            WSAQUERYSETW alignment;
            char bytes[8192];
        } storage;
        WSAQUERYSETW *result = (WSAQUERYSETW *)storage.bytes;
        DWORD result_length = (DWORD)sizeof(storage.bytes);
        ZeroMemory(&storage, sizeof(storage));
        result->dwSize = sizeof(*result);
        if (WSALookupServiceNextW(lookup,
                LUP_RETURN_NAME | LUP_RETURN_ADDR,
                &result_length, result) == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSA_E_NO_MORE && error_code != WSASERVICE_NOT_FOUND &&
                error_code != WSA_E_CANCELLED)
                mn_rfcomm_set_wsa_error(connection, "Bluetooth service enumeration",
                                        error_code);
            break;
        }
        if (result->lpcsaBuffer && result->dwNumberOfCsAddrs > 0) {
            const SOCKADDR_BTH *remote = NULL;
            int i;
            for (i = 0; i < (int)result->dwNumberOfCsAddrs; ++i) {
                const SOCKADDR *candidate =
                    result->lpcsaBuffer[i].LocalAddr.lpSockaddr;
                if (candidate && candidate->sa_family == AF_BTH) {
                    remote = (const SOCKADDR_BTH *)candidate;
                    break;
                }
            }
            if (remote && remote->btAddr != 0 && remote->port >= 1 &&
                remote->port <= 30) {
                ZeroMemory(out_device, sizeof(*out_device));
                out_device->address = (uint64_t)remote->btAddr;
                out_device->channel = (uint32_t)remote->port;
                if (!mn_rfcomm_to_utf8(out_device->name,
                                       sizeof(out_device->name),
                                       result->lpszServiceInstanceName) ||
                    !out_device->name[0]) {
                    (void)snprintf(out_device->name, sizeof(out_device->name),
                                   "%s", device_name ? device_name : "Bluetooth device");
                }
                found = 1;
                break;
            }
        }
    }
    WSALookupServiceEnd(lookup);
    return found;
}

#else

struct mn_rfcomm {
    mn_rfcomm_info info;
};

static void mn_rfcomm_set_error(mn_rfcomm *connection, const char *message) {
    if (!connection) return;
    (void)snprintf(connection->info.last_error,
                   sizeof(connection->info.last_error), "%s",
                   message ? message : "Bluetooth RFCOMM is unavailable");
}

#endif

int mn_rfcomm_supported(void) {
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}

mn_rfcomm *mn_rfcomm_create(void) {
    mn_rfcomm *connection = (mn_rfcomm *)calloc(1u, sizeof(*connection));
    if (!connection) return NULL;
#ifdef _WIN32
    {
        WSADATA data;
        int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            free(connection);
            return NULL;
        }
        connection->winsock_started = 1;
        connection->listener = INVALID_SOCKET;
        connection->socket = INVALID_SOCKET;
    }
#else
    mn_rfcomm_set_error(connection,
        "Native Bluetooth RFCOMM is currently supported only on Windows");
#endif
    return connection;
}

void mn_rfcomm_close(mn_rfcomm *connection) {
    if (!connection) return;
#ifdef _WIN32
    mn_rfcomm_unregister_service(connection);
    mn_rfcomm_close_socket(&connection->socket);
    mn_rfcomm_close_socket(&connection->listener);
#endif
    memset(&connection->info, 0, sizeof(connection->info));
    connection->info.state = MN_RFCOMM_IDLE;
#ifndef _WIN32
    mn_rfcomm_set_error(connection,
        "Native Bluetooth RFCOMM is currently supported only on Windows");
#endif
}

void mn_rfcomm_destroy(mn_rfcomm *connection) {
    if (!connection) return;
    mn_rfcomm_close(connection);
#ifdef _WIN32
    if (connection->winsock_started) WSACleanup();
#endif
    free(connection);
}

int mn_rfcomm_host(mn_rfcomm *connection, const char *service_name,
                   int secure_link) {
#ifdef _WIN32
    SOCKET listener;
    SOCKADDR_BTH local;
    int name_length;
    if (!connection || !service_name || !service_name[0]) return 0;
    mn_rfcomm_close(connection);
    name_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      service_name, -1,
                                      connection->service_name_w,
                                      MN_RFCOMM_NAME_LENGTH);
    if (name_length <= 0) {
        mn_rfcomm_set_error(connection, "Service name must be valid UTF-8");
        return 0;
    }
    listener = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (listener == INVALID_SOCKET) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth RFCOMM socket",
                                WSAGetLastError());
        return 0;
    }
    connection->listener = listener;
    if (!mn_rfcomm_set_security(listener, secure_link)) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth authentication/encryption options",
                                WSAGetLastError());
        mn_rfcomm_abort(connection);
        return 0;
    }
    ZeroMemory(&local, sizeof(local));
    local.addressFamily = AF_BTH;
    local.btAddr = 0;
    ZeroMemory(&local.serviceClassId, sizeof(local.serviceClassId));
    local.port = BT_PORT_ANY;
    if (bind(listener, (SOCKADDR *)&local, sizeof(local)) == SOCKET_ERROR) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth RFCOMM bind",
                                WSAGetLastError());
        mn_rfcomm_abort(connection);
        return 0;
    }
    if (listen(listener, 1) == SOCKET_ERROR) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth RFCOMM listen",
                                WSAGetLastError());
        mn_rfcomm_abort(connection);
        return 0;
    }
    if (!mn_rfcomm_nonblocking(listener)) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth listener non-blocking mode",
                                WSAGetLastError());
        mn_rfcomm_abort(connection);
        return 0;
    }
    {
        int address_length = sizeof(connection->local_address);
        ZeroMemory(&connection->local_address, sizeof(connection->local_address));
        if (getsockname(listener, (SOCKADDR *)&connection->local_address,
                        &address_length) == SOCKET_ERROR) {
            mn_rfcomm_set_wsa_error(connection, "Bluetooth channel lookup",
                                    WSAGetLastError());
            mn_rfcomm_abort(connection);
            return 0;
        }
    }
    if (connection->local_address.port < 1 ||
        connection->local_address.port > 30) {
        mn_rfcomm_set_error(connection, "Windows returned an invalid RFCOMM channel");
        mn_rfcomm_abort(connection);
        return 0;
    }
    connection->info.local_channel = (uint32_t)connection->local_address.port;
    if (!mn_rfcomm_register_service(connection)) {
        mn_rfcomm_abort(connection);
        return 0;
    }
    (void)snprintf(connection->info.service_name,
                   sizeof(connection->info.service_name), "%s", service_name);
    connection->info.state = MN_RFCOMM_LISTENING;
    connection->info.last_error[0] = '\0';
    return 1;
#else
    (void)service_name;
    (void)secure_link;
    mn_rfcomm_set_error(connection,
        "Native Bluetooth RFCOMM is currently supported only on Windows");
    return 0;
#endif
}

size_t mn_rfcomm_discover(mn_rfcomm *connection,
                          mn_rfcomm_device *devices, size_t capacity) {
#ifdef _WIN32
    WSAQUERYSETW restrictions;
    HANDLE inquiry = NULL;
    size_t found_count = 0;
    if (!connection || (!devices && capacity != 0)) return 0;
    connection->info.last_error[0] = '\0';
    ZeroMemory(&restrictions, sizeof(restrictions));
    restrictions.dwSize = sizeof(restrictions);
    restrictions.dwNameSpace = NS_BTH;
    if (WSALookupServiceBeginW(&restrictions,
            LUP_CONTAINERS | LUP_FLUSHCACHE, &inquiry) == SOCKET_ERROR) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth device inquiry",
                                WSAGetLastError());
        return 0;
    }
    for (;;) {
        union {
            WSAQUERYSETW alignment;
            char bytes[8192];
        } storage;
        WSAQUERYSETW *result = (WSAQUERYSETW *)storage.bytes;
        DWORD result_length = (DWORD)sizeof(storage.bytes);
        uint64_t device_address = 0;
        char device_name[MN_RFCOMM_NAME_LENGTH] = "Bluetooth device";
        ZeroMemory(&storage, sizeof(storage));
        result->dwSize = sizeof(*result);
        if (WSALookupServiceNextW(inquiry, LUP_RETURN_NAME | LUP_RETURN_ADDR,
                                  &result_length, result) == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code != WSA_E_NO_MORE && error_code != WSASERVICE_NOT_FOUND &&
                error_code != WSA_E_CANCELLED)
                mn_rfcomm_set_wsa_error(connection, "Bluetooth device enumeration",
                                        error_code);
            break;
        }
        if (result->lpcsaBuffer && result->dwNumberOfCsAddrs > 0) {
            int i;
            for (i = 0; i < (int)result->dwNumberOfCsAddrs; ++i) {
                const SOCKADDR *candidate =
                    result->lpcsaBuffer[i].LocalAddr.lpSockaddr;
                if (candidate && candidate->sa_family == AF_BTH) {
                    device_address =
                        (uint64_t)((const SOCKADDR_BTH *)candidate)->btAddr;
                    break;
                }
            }
        }
        if (result->lpszServiceInstanceName)
            (void)mn_rfcomm_to_utf8(device_name, sizeof(device_name),
                                    result->lpszServiceInstanceName);
        if (device_address != 0) {
            mn_rfcomm_device device;
            if (mn_rfcomm_query_device(connection, device_address,
                                       device_name, &device)) {
                size_t i;
                int duplicate = 0;
                for (i = 0; i < found_count && i < capacity; ++i) {
                    if (devices && devices[i].address == device.address) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate) {
                    if (devices && found_count < capacity)
                        devices[found_count] = device;
                    ++found_count;
                }
            }
        }
    }
    WSALookupServiceEnd(inquiry);
    if (found_count == 0 && connection->info.last_error[0] == '\0')
        mn_rfcomm_set_error(connection,
            "No visible nearby device is advertising the MultiNet RFCOMM service");
    return found_count;
#else
    (void)devices;
    (void)capacity;
    mn_rfcomm_set_error(connection,
        "Native Bluetooth RFCOMM is currently supported only on Windows");
    return 0;
#endif
}

int mn_rfcomm_join(mn_rfcomm *connection, const mn_rfcomm_device *device,
                   int secure_link) {
#ifdef _WIN32
    SOCKADDR_BTH remote;
    SOCKET socket_value;
    int connect_result;
    if (!connection || !device || device->address == 0 ||
        device->channel < 1 || device->channel > 30) return 0;
    mn_rfcomm_close(connection);
    socket_value = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (socket_value == INVALID_SOCKET) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth RFCOMM socket",
                                WSAGetLastError());
        return 0;
    }
    connection->socket = socket_value;
    if (!mn_rfcomm_set_security(socket_value, secure_link)) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth authentication/encryption options",
                                WSAGetLastError());
        mn_rfcomm_drop_peer(connection);
        return 0;
    }
    if (!mn_rfcomm_nonblocking(socket_value)) {
        mn_rfcomm_set_wsa_error(connection, "Bluetooth client non-blocking mode",
                                WSAGetLastError());
        mn_rfcomm_drop_peer(connection);
        return 0;
    }
    ZeroMemory(&remote, sizeof(remote));
    remote.addressFamily = AF_BTH;
    remote.btAddr = (BTH_ADDR)device->address;
    remote.serviceClassId = MN_RFCOMM_SERVICE_UUID;
    remote.port = device->channel;
    connect_result = connect(socket_value, (SOCKADDR *)&remote, sizeof(remote));
    if (connect_result == SOCKET_ERROR) {
        int error_code = WSAGetLastError();
        if (error_code != WSAEWOULDBLOCK && error_code != WSAEINPROGRESS &&
            error_code != WSAEALREADY) {
            mn_rfcomm_set_wsa_error(connection, "Bluetooth RFCOMM connect",
                                    error_code);
            mn_rfcomm_drop_peer(connection);
            return 0;
        }
        connection->info.state = MN_RFCOMM_CONNECTING;
    } else {
        connection->info.state = MN_RFCOMM_CONNECTED;
    }
    connection->info.remote_address = device->address;
    (void)snprintf(connection->info.service_name,
                   sizeof(connection->info.service_name), "%s",
                   device->name[0] ? device->name : "Bluetooth device");
    connection->info.last_error[0] = '\0';
    return 1;
#else
    (void)device;
    (void)secure_link;
    mn_rfcomm_set_error(connection,
        "Native Bluetooth RFCOMM is currently supported only on Windows");
    return 0;
#endif
}

int mn_rfcomm_update(mn_rfcomm *connection) {
#ifdef _WIN32
    if (!connection) return 0;
    if (connection->listener != INVALID_SOCKET &&
        connection->socket == INVALID_SOCKET) {
        SOCKADDR_BTH remote;
        int remote_length = sizeof(remote);
        SOCKET accepted = accept(connection->listener, (SOCKADDR *)&remote,
                                 &remote_length);
        if (accepted != INVALID_SOCKET) {
            if (!mn_rfcomm_nonblocking(accepted)) {
                mn_rfcomm_set_wsa_error(connection, "Accepted Bluetooth socket non-blocking mode",
                                        WSAGetLastError());
                closesocket(accepted);
            } else {
                connection->socket = accepted;
                connection->info.remote_address = (uint64_t)remote.btAddr;
                connection->info.state = MN_RFCOMM_CONNECTED;
                connection->info.last_error[0] = '\0';
            }
        } else {
            int error_code = WSAGetLastError();
            if (error_code != WSAEWOULDBLOCK)
                mn_rfcomm_set_wsa_error(connection, "Bluetooth accept", error_code);
        }
    }
    if (connection->info.state == MN_RFCOMM_CONNECTING &&
        connection->socket != INVALID_SOCKET) {
        fd_set write_set, error_set;
        TIMEVAL timeout;
        int select_result;
        int socket_error = 0;
        int error_length = sizeof(socket_error);
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
        FD_SET(connection->socket, &write_set);
        FD_SET(connection->socket, &error_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        select_result = select(0, NULL, &write_set, &error_set, &timeout);
        if (select_result == SOCKET_ERROR) {
            mn_rfcomm_set_wsa_error(connection, "Bluetooth connection poll",
                                    WSAGetLastError());
            mn_rfcomm_drop_peer(connection);
            return 0;
        }
        if (select_result > 0 &&
            (FD_ISSET(connection->socket, &write_set) ||
             FD_ISSET(connection->socket, &error_set))) {
            if (getsockopt(connection->socket, SOL_SOCKET, SO_ERROR,
                           (char *)&socket_error, &error_length) == SOCKET_ERROR)
                socket_error = WSAGetLastError();
            if (socket_error != 0) {
                mn_rfcomm_set_wsa_error(connection, "Bluetooth RFCOMM connect",
                                        socket_error);
                mn_rfcomm_drop_peer(connection);
                return 0;
            }
            connection->info.state = MN_RFCOMM_CONNECTED;
            connection->info.last_error[0] = '\0';
        }
    }
    return 1;
#else
    (void)connection;
    return 0;
#endif
}

int mn_rfcomm_read(mn_rfcomm *connection, void *buffer, size_t capacity,
                   size_t *out_transferred) {
#ifdef _WIN32
    int received;
    int request;
    if (out_transferred) *out_transferred = 0;
    if (!connection || !buffer || capacity == 0 ||
        connection->info.state != MN_RFCOMM_CONNECTED ||
        connection->socket == INVALID_SOCKET) return 0;
    request = capacity > (size_t)INT_MAX ? INT_MAX : (int)capacity;
    received = recv(connection->socket, (char *)buffer, request, 0);
    if (received > 0) {
        if (out_transferred) *out_transferred = (size_t)received;
        return 1;
    }
    if (received == 0) {
        mn_rfcomm_drop_peer(connection);
        mn_rfcomm_set_error(connection, "Bluetooth peer disconnected");
        return 0;
    }
    {
        int error_code = WSAGetLastError();
        if (error_code == WSAEWOULDBLOCK) return 1;
        mn_rfcomm_drop_peer(connection);
        mn_rfcomm_set_wsa_error(connection, "Bluetooth stream read", error_code);
    }
    return 0;
#else
    (void)buffer;
    (void)capacity;
    if (out_transferred) *out_transferred = 0;
    mn_rfcomm_set_error(connection,
        "Native Bluetooth RFCOMM is currently supported only on Windows");
    return 0;
#endif
}

int mn_rfcomm_write(mn_rfcomm *connection, const void *data, size_t size,
                    size_t *out_transferred) {
#ifdef _WIN32
    int sent;
    int request;
    if (out_transferred) *out_transferred = 0;
    if (!connection || (!data && size != 0) ||
        connection->info.state != MN_RFCOMM_CONNECTED ||
        connection->socket == INVALID_SOCKET) return 0;
    if (size == 0) return 1;
    request = size > (size_t)INT_MAX ? INT_MAX : (int)size;
    sent = send(connection->socket, (const char *)data, request, 0);
    if (sent > 0) {
        if (out_transferred) *out_transferred = (size_t)sent;
        return 1;
    }
    if (sent == 0) return 1;
    {
        int error_code = WSAGetLastError();
        if (error_code == WSAEWOULDBLOCK) return 1;
        mn_rfcomm_drop_peer(connection);
        mn_rfcomm_set_wsa_error(connection, "Bluetooth stream write", error_code);
    }
    return 0;
#else
    (void)data;
    (void)size;
    if (out_transferred) *out_transferred = 0;
    mn_rfcomm_set_error(connection,
        "Native Bluetooth RFCOMM is currently supported only on Windows");
    return 0;
#endif
}

void mn_rfcomm_get_info(const mn_rfcomm *connection, mn_rfcomm_info *out_info) {
    if (!out_info) return;
    memset(out_info, 0, sizeof(*out_info));
    if (connection) *out_info = connection->info;
    else {
        (void)snprintf(out_info->last_error, sizeof(out_info->last_error),
                       "Invalid Bluetooth RFCOMM instance");
    }
}
