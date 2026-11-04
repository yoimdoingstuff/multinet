/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
/* MultiNet C11 implementation. The public API is declared in multinet.h. */
#include "multinet.h"
#include "multinet_crypto.h"

#include <math.h>
#include <float.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <bcrypt.h>
typedef SOCKET mn_socket;
typedef int mn_socklen;
#define MN_BAD_SOCKET INVALID_SOCKET
#define mn_close_socket closesocket
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int mn_socket;
typedef socklen_t mn_socklen;
#define MN_BAD_SOCKET (-1)
#define mn_close_socket close
#endif

#define MN_MAGIC 0x4D4E4554u /* "MNET" */
#define MN_PROTOCOL_VERSION 4u
#define MN_HEADER_SIZE 16u
#define MN_MAX_DATAGRAM_SIZE 1472u
#define MN_EVENT_QUEUE_CAPACITY 128u
#define MN_WIRE_PLAYER_SIZE 88u
#define MN_OFFER_SIZE (2u + MN_ROOM_NAME_LENGTH + MN_PLAYER_NAME_LENGTH)
#define MN_WELCOME_SIZE (4u + 8u + 1u + MN_ROOM_NAME_LENGTH)
#define MN_CONNECT_TIMEOUT 10.0f
#define MN_MAX_RELIABLE_PENDING 64u
#define MN_RELIABLE_RETRY_SECONDS 0.30f
#define MN_RELIABLE_MAX_ATTEMPTS 8u
#define MN_PING_TIMEOUT_SECONDS 5.0f
#define MN_MDNS_PORT 5353u
#define MN_MDNS_MAX_PACKET 1400u
#define MN_RELIABLE_SEEN_IDS 64u
#define MN_SECURE_REPLAY_CAPACITY 64u
#define MN_SECURE_NONCE_BYTES 12u
#define MN_SECURE_TAG_BYTES 16u
#define MN_SECURE_OVERHEAD (MN_SECURE_NONCE_BYTES + MN_SECURE_TAG_BYTES)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(float) == 4, "MultiNet requires 32-bit floats");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "MultiNet requires IEEE-754 binary32 floats");
#endif

enum mn_packet_type {
    MN_PKT_DISCOVER = 1,
    MN_PKT_OFFER,
    MN_PKT_JOIN,
    MN_PKT_WELCOME,
    MN_PKT_STATE,
    MN_PKT_SNAPSHOT,
    MN_PKT_LEAVE,
    MN_PKT_START,
    MN_PKT_MESSAGE,
    MN_PKT_RELIABLE,
    MN_PKT_RELIABLE_ACK,
    MN_PKT_PING,
    MN_PKT_PONG,
    MN_PKT_PAIRING_DENIED,
    MN_PKT_SECURE,
    MN_PKT_PAIRING_CHALLENGE,
    MN_PKT_PAIRING_PROOF
};

typedef struct mn_peer {
    int active;
    int pending_approval;
    int challenge_pending;
    uint8_t device_id[MN_DEVICE_ID_BYTES];
    uint8_t device_secret[MN_DEVICE_SECRET_BYTES];
    uint8_t challenge_nonce[32];
    uint32_t id;
    uint64_t token;
    float silence;
    float state_cooldown;
    float message_cooldown;
    uint32_t last_state_sequence;
    int has_state_sequence;
    struct sockaddr_in address;
    mn_player player;
} mn_peer;


typedef struct mn_reliable_entry {
    int active;
    uint32_t peer_id;       /* 0 means the client is sending to its host. */
    uint32_t sender_id;
    uint32_t target_id;
    uint32_t message_id;
    uint16_t channel;
    uint16_t data_size;
    uint8_t attempts;
    float retry_elapsed;
    uint8_t data[MN_MAX_MESSAGE_BYTES];
} mn_reliable_entry;

typedef struct mn_seen_reliable {
    uint32_t sender_id;
    uint32_t message_ids[MN_RELIABLE_SEEN_IDS];
    uint8_t count;
    uint8_t next;
    uint32_t age;
} mn_seen_reliable;

typedef struct mn_secure_replay {
    int active;
    struct sockaddr_in address;
    uint64_t nonce_prefix;
    uint32_t highest_counter;
    uint64_t seen_window;
    uint32_t age;
} mn_secure_replay;

struct mn_instance {
    int ready;
    mn_socket socket;
    mn_socket mdns_socket;
    mn_config config;
    mn_info info;
    mn_player local_player;
    mn_player_state_limits state_limits;
    mn_player_state_validator state_validator;
    void *state_validator_user_data;
    int join_allowlist_enabled;
    size_t join_allowlist_count;
    char join_allowlist[MN_MAX_JOIN_ALLOWLIST][48];
    uint8_t device_id[MN_DEVICE_ID_BYTES];
    uint8_t device_secret[MN_DEVICE_SECRET_BYTES];
    int pairing_required;
    size_t paired_device_count;
    uint8_t paired_devices[MN_MAX_PAIRED_DEVICES][MN_DEVICE_ID_BYTES];
    uint8_t paired_device_secrets[MN_MAX_PAIRED_DEVICES][MN_DEVICE_SECRET_BYTES];
    int encryption_enabled;
    uint8_t room_key[MN_ROOM_KEY_BYTES];
    uint64_t secure_nonce_prefix;
    uint32_t secure_nonce_counter;
    uint32_t secure_replay_age;
    mn_secure_replay secure_replays[MN_SECURE_REPLAY_CAPACITY];
    mn_player players[MN_MAX_PLAYERS];
    mn_room rooms[MN_MAX_ROOMS];
    size_t room_count;
    mn_peer peers[MN_MAX_PLAYERS];
    struct sockaddr_in server;
    struct sockaddr_in discovery_target;
    char bound_ipv4[48];
    int discovery_target_valid;
    int discovery_mdns;
    int discovery_auto;
    uint64_t session_token;
    uint32_t next_player_id;
    uint16_t local_port;
    float discovery_left;
    float discovery_tick;
    float heartbeat_tick;
    float snapshot_tick;
    float connect_elapsed;
    float server_silence;
    uint32_t next_state_sequence;
    uint32_t snapshot_sequence;
    uint32_t last_snapshot_sequence;
    int has_snapshot_sequence;
    uint32_t next_reliable_id;
    uint32_t reliable_seen_age;
    uint32_t ping_sequence;
    uint32_t ping_pending_id;
    uint32_t ping_pending_target;
    float ping_elapsed;
    mn_reliable_entry reliable[MN_MAX_RELIABLE_PENDING];
    mn_seen_reliable reliable_seen[MN_MAX_PLAYERS];
    mn_event events[MN_EVENT_QUEUE_CAPACITY];
    size_t event_head;
    size_t event_count;
};

static void mn_emit_reliable_status(mn_instance *instance, mn_event_type type,
                                    const mn_reliable_entry *entry,
                                    uint32_t peer_id, const char *message);
static void mn_secure_zero(void *memory, size_t size);

static void mn_copy_text(char *destination, size_t capacity, const char *source) {
    size_t length;
    if (!destination || capacity == 0) return;
    if (!source) source = "";
    length = strlen(source);
    if (length >= capacity) length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void mn_push_event(mn_instance *instance, const mn_event *event) {
    size_t slot;
    if (!instance || !event) return;
    if (instance->event_count == MN_EVENT_QUEUE_CAPACITY) {
        instance->event_head = (instance->event_head + 1u) % MN_EVENT_QUEUE_CAPACITY;
        instance->event_count--;
    }
    slot = (instance->event_head + instance->event_count) % MN_EVENT_QUEUE_CAPACITY;
    instance->events[slot] = *event;
    instance->event_count++;
}

static void mn_emit_simple_event(mn_instance *instance, mn_event_type type,
                                 const char *message, uint32_t player_id) {
    mn_event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.player_id = player_id;
    mn_copy_text(event.text, sizeof(event.text), message);
    mn_push_event(instance, &event);
}



static void mn_set_error(mn_instance *instance, const char *message) {
    if (!instance) return;
    mn_copy_text(instance->info.last_error, sizeof(instance->info.last_error), message);
    mn_emit_simple_event(instance, MN_EVENT_ERROR, message, 0);
}

static void mn_write_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}
static void mn_write_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}
static void mn_write_u64(uint8_t *out, uint64_t value) {
    mn_write_u32(out, (uint32_t)(value >> 32));
    mn_write_u32(out + 4, (uint32_t)value);
}
static uint16_t mn_read_u16(const uint8_t *in) {
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}
static uint32_t mn_read_u32(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
        ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}
static uint64_t mn_read_u64(const uint8_t *in) {
    return ((uint64_t)mn_read_u32(in) << 32) | mn_read_u32(in + 4);
}
static void mn_write_float(uint8_t *out, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    mn_write_u32(out, bits);
}
static float mn_read_float(const uint8_t *in) {
    uint32_t bits = mn_read_u32(in);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int mn_random_bytes(uint8_t *out, size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, out, (ULONG)size,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE *random_file = fopen("/dev/urandom", "rb");
    size_t read_size;
    if (!random_file) return 0;
    read_size = fread(out, 1, size, random_file);
    fclose(random_file);
    return read_size == size;
#endif
}

static int mn_random_u32(uint32_t *out) {
    unsigned attempts;
    if (!out) return 0;
    for (attempts = 0; attempts < 4; ++attempts) {
        if (!mn_random_bytes((uint8_t *)out, sizeof(*out))) return 0;
        if (*out != 0) return 1;
    }
    return 0;
}
static int mn_random_u64(uint64_t *out) {
    unsigned attempts;
    if (!out) return 0;
    for (attempts = 0; attempts < 4; ++attempts) {
        if (!mn_random_bytes((uint8_t *)out, sizeof(*out))) return 0;
        if (*out != 0) return 1;
    }
    return 0;
}

static int mn_set_nonblocking(mn_socket socket_handle) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket_handle, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(socket_handle, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static mn_socket mn_open_socket_on(mn_instance *instance, uint16_t port,
                                      const char *local_ipv4) {
    mn_socket socket_handle;
    struct sockaddr_in address;
    mn_socklen address_length;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == MN_BAD_SOCKET) {
        mn_set_error(instance, "Could not create UDP socket");
        return MN_BAD_SOCKET;
    }
#ifdef _WIN32
    {
        int exclusive = 1;
        (void)setsockopt(socket_handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         (const char *)&exclusive, (int)sizeof(exclusive));
    }
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (local_ipv4 && local_ipv4[0]) {
        if (inet_pton(AF_INET, local_ipv4, &address.sin_addr) != 1) {
            mn_set_error(instance, "The selected local interface is not a valid IPv4 address");
            mn_close_socket(socket_handle);
            return MN_BAD_SOCKET;
        }
    } else {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(socket_handle, (struct sockaddr *)&address,
             (mn_socklen)sizeof(address)) != 0) {
        mn_set_error(instance, "Could not bind the UDP socket to the selected interface/port");
        mn_close_socket(socket_handle);
        return MN_BAD_SOCKET;
    }
    if (!mn_set_nonblocking(socket_handle)) {
        mn_set_error(instance, "Could not make the UDP socket non-blocking");
        mn_close_socket(socket_handle);
        return MN_BAD_SOCKET;
    }
    if (instance->config.enable_broadcast) {
        int broadcast_enabled = 1;
        (void)setsockopt(socket_handle, SOL_SOCKET, SO_BROADCAST,
                         (const char *)&broadcast_enabled, (mn_socklen)sizeof(broadcast_enabled));
    }
    memset(&address, 0, sizeof(address));
    address_length = (mn_socklen)sizeof(address);
    if (getsockname(socket_handle, (struct sockaddr *)&address, &address_length) == 0)
        instance->local_port = ntohs(address.sin_port);
    mn_copy_text(instance->bound_ipv4, sizeof(instance->bound_ipv4),
                 local_ipv4 && local_ipv4[0] ? local_ipv4 : "");
    return socket_handle;
}

static mn_socket mn_open_socket(mn_instance *instance, uint16_t port) {
    return mn_open_socket_on(instance, port, NULL);
}


static mn_socket mn_open_mdns_socket(mn_instance *instance) {
    mn_socket socket_handle;
    struct sockaddr_in address;
    struct ip_mreq membership;
    int reuse = 1;
    int ttl = 255;
    int membership_joined = 0;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == MN_BAD_SOCKET) return MN_BAD_SOCKET;
    (void)setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&reuse, (mn_socklen)sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(MN_MDNS_PORT);
    if (bind(socket_handle, (struct sockaddr *)&address,
             (mn_socklen)sizeof(address)) != 0) {
        mn_close_socket(socket_handle);
        return MN_BAD_SOCKET;
    }
    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char *)&membership, (mn_socklen)sizeof(membership)) == 0)
        membership_joined = 1;
    {
        mn_interface interfaces[64];
        size_t count = mn_get_interfaces(interfaces, 64);
        size_t i;
        for (i = 0; i < count && i < 64; ++i) {
            struct in_addr local_address;
            if (interfaces[i].type == MN_INTERFACE_LOOPBACK ||
                !interfaces[i].address[0] ||
                inet_pton(AF_INET, interfaces[i].address, &local_address) != 1)
                continue;
            membership.imr_interface = local_address;
            if (setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           (const char *)&membership,
                           (mn_socklen)sizeof(membership)) == 0)
                membership_joined = 1;
        }
    }
    if (!membership_joined) {
        mn_close_socket(socket_handle);
        return MN_BAD_SOCKET;
    }
    (void)setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_TTL,
                     (const char *)&ttl, (mn_socklen)sizeof(ttl));
    if (!mn_set_nonblocking(socket_handle)) {
        mn_close_socket(socket_handle);
        return MN_BAD_SOCKET;
    }
    (void)instance;
    return socket_handle;
}

static void mn_close_current_socket(mn_instance *instance) {
    if (instance->socket != MN_BAD_SOCKET) mn_close_socket(instance->socket);
    instance->socket = MN_BAD_SOCKET;
    instance->local_port = 0;
    instance->bound_ipv4[0] = '\0';
}


static int mn_text_contains_ci(const char *text, const char *needle) {
    size_t i, j;
    if (!text || !needle || !needle[0]) return 0;
    for (i = 0; text[i]; ++i) {
        for (j = 0; needle[j] && text[i + j] &&
             tolower((unsigned char)text[i + j]) == tolower((unsigned char)needle[j]); ++j) { }
        if (!needle[j]) return 1;
    }
    return 0;
}

static void mn_store_interface(mn_interface *out, size_t capacity, size_t *count,
                               const mn_interface *value) {
    if (!count || !value) return;
    if (out && *count < capacity) out[*count] = *value;
    ++(*count);
}

size_t mn_get_interfaces(mn_interface *interfaces, size_t capacity) {
    size_t count = 0;
#ifdef _WIN32
    ULONG buffer_size = 15000;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    IP_ADAPTER_ADDRESSES *adapters = (IP_ADAPTER_ADDRESSES *)malloc(buffer_size);
    ULONG status;
    if (!adapters) return 0;
    status = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &buffer_size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        IP_ADAPTER_ADDRESSES *grown = (IP_ADAPTER_ADDRESSES *)realloc(adapters, buffer_size);
        if (!grown) { free(adapters); return 0; }
        adapters = grown;
        status = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &buffer_size);
    }
    if (status != NO_ERROR) { free(adapters); return 0; }
    {
        IP_ADAPTER_ADDRESSES *adapter;
        for (adapter = adapters; adapter; adapter = adapter->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *unicast;
            if (adapter->OperStatus != IfOperStatusUp) continue;
            for (unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                struct sockaddr_in *address;
                mn_interface item;
                uint32_t ip_host, mask_host, broadcast_host;
                unsigned prefix;
                char display_name[160];
                if (!unicast->Address.lpSockaddr ||
                    unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                address = (struct sockaddr_in *)unicast->Address.lpSockaddr;
                memset(&item, 0, sizeof(item));
                if (!inet_ntop(AF_INET, &address->sin_addr, item.address,
                               sizeof(item.address))) continue;
                prefix = unicast->OnLinkPrefixLength > 32 ? 32u : unicast->OnLinkPrefixLength;
                mask_host = prefix == 0 ? 0u : (0xFFFFFFFFu << (32u - prefix));
                ip_host = ntohl(address->sin_addr.s_addr);
                broadcast_host = (ip_host & mask_host) | (~mask_host);
                {
                    struct in_addr mask_addr, broadcast_addr;
                    mask_addr.s_addr = htonl(mask_host);
                    broadcast_addr.s_addr = htonl(broadcast_host);
                    (void)inet_ntop(AF_INET, &mask_addr, item.netmask, sizeof(item.netmask));
                    (void)inet_ntop(AF_INET, &broadcast_addr, item.broadcast, sizeof(item.broadcast));
                }
                display_name[0] = '\0';
                if (adapter->FriendlyName)
                    WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                        display_name, (int)sizeof(display_name), NULL, NULL);
                else
                    display_name[0] = '\0';
                mn_copy_text(item.name, sizeof(item.name),
                             adapter->AdapterName ? adapter->AdapterName : "adapter");
                mn_copy_text(item.description, sizeof(item.description),
                             display_name[0] ? display_name :
                             (adapter->Description ? "Network adapter" : item.name));
                item.index = adapter->IfIndex;
                if (mn_text_contains_ci(item.description, "wi-fi direct") ||
                    mn_text_contains_ci(item.description, "wifi direct") ||
                    mn_text_contains_ci(item.description, "wireless direct"))
                    item.type = MN_INTERFACE_WIFI_DIRECT;
                else if (mn_text_contains_ci(item.description, "bluetooth") ||
                         mn_text_contains_ci(item.name, "bth"))
                    item.type = MN_INTERFACE_BLUETOOTH_PAN;
                else if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    item.type = MN_INTERFACE_LOOPBACK;
                else if (adapter->IfType == IF_TYPE_IEEE80211)
                    item.type = MN_INTERFACE_WIFI;
                else if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD)
                    item.type = MN_INTERFACE_ETHERNET;
                else
                    item.type = MN_INTERFACE_OTHER;
                mn_store_interface(interfaces, capacity, &count, &item);
            }
        }
    }
    free(adapters);
#else
    {
        struct ifaddrs *all = NULL, *entry;
        if (getifaddrs(&all) != 0) return 0;
        for (entry = all; entry; entry = entry->ifa_next) {
            mn_interface item;
            struct sockaddr_in *address;
            uint32_t ip_host, mask_host, broadcast_host;
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
                !(entry->ifa_flags & IFF_UP)) continue;
            address = (struct sockaddr_in *)entry->ifa_addr;
            memset(&item, 0, sizeof(item));
            if (!inet_ntop(AF_INET, &address->sin_addr, item.address,
                           sizeof(item.address))) continue;
            mn_copy_text(item.name, sizeof(item.name), entry->ifa_name);
            mn_copy_text(item.description, sizeof(item.description), entry->ifa_name);
            item.index = if_nametoindex(entry->ifa_name);
            if (entry->ifa_netmask) {
                struct in_addr *mask = &((struct sockaddr_in *)entry->ifa_netmask)->sin_addr;
                (void)inet_ntop(AF_INET, mask, item.netmask, sizeof(item.netmask));
                ip_host = ntohl(address->sin_addr.s_addr);
                mask_host = ntohl(mask->s_addr);
                broadcast_host = (ip_host & mask_host) | (~mask_host);
                {
                    struct in_addr broadcast_addr;
                    broadcast_addr.s_addr = htonl(broadcast_host);
                    (void)inet_ntop(AF_INET, &broadcast_addr, item.broadcast, sizeof(item.broadcast));
                }
            }
            if (entry->ifa_flags & IFF_LOOPBACK)
                item.type = MN_INTERFACE_LOOPBACK;
            else if (mn_text_contains_ci(entry->ifa_name, "bnep") ||
                     mn_text_contains_ci(entry->ifa_name, "bluetooth") ||
                     mn_text_contains_ci(entry->ifa_name, "bt-pan"))
                item.type = MN_INTERFACE_BLUETOOTH_PAN;
            else if (mn_text_contains_ci(entry->ifa_name, "p2p") ||
                     mn_text_contains_ci(entry->ifa_name, "wfd"))
                item.type = MN_INTERFACE_WIFI_DIRECT;
            else {
                char wireless_path[256];
                (void)snprintf(wireless_path, sizeof(wireless_path),
                               "/sys/class/net/%s/wireless", entry->ifa_name);
                if (access(wireless_path, F_OK) == 0 ||
                    strncmp(entry->ifa_name, "wl", 2) == 0 ||
                    strncmp(entry->ifa_name, "wlan", 4) == 0)
                    item.type = MN_INTERFACE_WIFI;
                else if (strncmp(entry->ifa_name, "eth", 3) == 0 ||
                         strncmp(entry->ifa_name, "en", 2) == 0)
                    item.type = MN_INTERFACE_ETHERNET;
                else
                    item.type = MN_INTERFACE_OTHER;
            }
            mn_store_interface(interfaces, capacity, &count, &item);
        }
        freeifaddrs(all);
    }
#endif
    return count;
}

static int mn_rebind_socket(mn_instance *instance, const char *local_ipv4) {
    char previous[48];
    mn_socket replacement;
    const char *requested = local_ipv4 && local_ipv4[0] ? local_ipv4 : "";
    if (strcmp(instance->bound_ipv4, requested) == 0) return 1;
    mn_copy_text(previous, sizeof(previous), instance->bound_ipv4);
    mn_close_current_socket(instance);
    replacement = mn_open_socket_on(instance, 0, requested);
    if (replacement == MN_BAD_SOCKET) {
        instance->socket = mn_open_socket_on(instance, 0, previous[0] ? previous : NULL);
        instance->info.local_port = instance->local_port;
        return 0;
    }
    instance->socket = replacement;
    instance->info.local_port = instance->local_port;
    return 1;
}

static int mn_same_address(const struct sockaddr_in *left,
                           const struct sockaddr_in *right) {
    return left->sin_addr.s_addr == right->sin_addr.s_addr &&
           left->sin_port == right->sin_port;
}

static int mn_normalize_ipv4(const char *input, char output[48]) {
    struct in_addr parsed;
    if (!input || !input[0] || !output) return 0;
    if (inet_pton(AF_INET, input, &parsed) != 1) return 0;
    return inet_ntop(AF_INET, &parsed, output, 48) != NULL;
}

static int mn_join_source_is_allowed(const mn_instance *instance,
                                     const struct sockaddr_in *from) {
    char address[48];
    size_t i;
    if (!instance->join_allowlist_enabled) return 1;
    if (!inet_ntop(AF_INET, &from->sin_addr, address, sizeof(address)))
        return 0;
    for (i = 0; i < instance->join_allowlist_count; ++i) {
        if (strcmp(instance->join_allowlist[i], address) == 0)
            return 1;
    }
    return 0;
}

static mn_peer *mn_find_peer_address(mn_instance *instance,
                                     const struct sockaddr_in *address) {
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if ((instance->peers[i].active || instance->peers[i].pending_approval) &&
            mn_same_address(&instance->peers[i].address, address))
            return &instance->peers[i];
    }
    return NULL;
}
static mn_peer *mn_find_peer_token(mn_instance *instance, uint64_t token) {
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active && instance->peers[i].token == token)
            return &instance->peers[i];
    }
    return NULL;
}
static mn_peer *mn_find_peer_id(mn_instance *instance, uint32_t player_id) {
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active && instance->peers[i].id == player_id)
            return &instance->peers[i];
    }
    return NULL;
}

static void mn_encode_player(uint8_t *out, const mn_player *player) {
    size_t offset = 0;
    size_t i;
    mn_write_u32(out + offset, player->id); offset += 4;
    mn_write_u32(out + offset, (uint32_t)player->score); offset += 4;
    mn_write_u32(out + offset, player->flags); offset += 4;
    mn_write_u32(out + offset, player->colour_rgba); offset += 4;
    for (i = 0; i < 4; ++i) { mn_write_u16(out + offset, player->appearance[i]); offset += 2; }
    memset(out + offset, 0, MN_PLAYER_NAME_LENGTH);
    mn_copy_text((char *)(out + offset), MN_PLAYER_NAME_LENGTH, player->name);
    offset += MN_PLAYER_NAME_LENGTH;
    mn_write_float(out + offset, player->position.x); offset += 4;
    mn_write_float(out + offset, player->position.y); offset += 4;
    mn_write_float(out + offset, player->position.z); offset += 4;
    mn_write_float(out + offset, player->rotation.x); offset += 4;
    mn_write_float(out + offset, player->rotation.y); offset += 4;
    mn_write_float(out + offset, player->rotation.z); offset += 4;
    mn_write_float(out + offset, player->rotation.w); offset += 4;
    mn_write_float(out + offset, player->velocity.x); offset += 4;
    mn_write_float(out + offset, player->velocity.y); offset += 4;
    mn_write_float(out + offset, player->velocity.z); offset += 4;
}

static int mn_sequence_is_newer(uint32_t candidate, uint32_t previous) {
    uint32_t difference = candidate - previous;
    return difference != 0u && difference < 0x80000000u;
}

static int mn_float_is_valid(float value) {
    return isfinite((double)value) && fabs((double)value) <= 1000000.0;
}
static int mn_player_is_valid(const mn_player *player) {
    return mn_float_is_valid(player->position.x) &&
           mn_float_is_valid(player->position.y) &&
           mn_float_is_valid(player->position.z) &&
           mn_float_is_valid(player->rotation.x) &&
           mn_float_is_valid(player->rotation.y) &&
           mn_float_is_valid(player->rotation.z) &&
           mn_float_is_valid(player->rotation.w) &&
           mn_float_is_valid(player->velocity.x) &&
           mn_float_is_valid(player->velocity.y) &&
           mn_float_is_valid(player->velocity.z);
}

static int mn_player_within_state_limits(const mn_instance *instance,
                                         const mn_player *player) {
    const mn_player_state_limits *limits = &instance->state_limits;
    const float position_limit = limits->max_abs_position;
    const float velocity_limit = limits->max_abs_velocity;
    if (position_limit > 0.0f &&
        (fabs((double)player->position.x) > (double)position_limit ||
         fabs((double)player->position.y) > (double)position_limit ||
         fabs((double)player->position.z) > (double)position_limit))
        return 0;
    if (velocity_limit > 0.0f &&
        (fabs((double)player->velocity.x) > (double)velocity_limit ||
         fabs((double)player->velocity.y) > (double)velocity_limit ||
         fabs((double)player->velocity.z) > (double)velocity_limit))
        return 0;
    return 1;
}

static int mn_decode_player(mn_player *player, const uint8_t *in) {
    size_t offset = 0;
    size_t i;
    memset(player, 0, sizeof(*player));
    player->id = mn_read_u32(in + offset); offset += 4;
    player->score = (int32_t)mn_read_u32(in + offset); offset += 4;
    player->flags = mn_read_u32(in + offset); offset += 4;
    player->colour_rgba = mn_read_u32(in + offset); offset += 4;
    for (i = 0; i < 4; ++i) { player->appearance[i] = mn_read_u16(in + offset); offset += 2; }
    memcpy(player->name, in + offset, MN_PLAYER_NAME_LENGTH);
    player->name[MN_PLAYER_NAME_LENGTH - 1] = '\0';
    offset += MN_PLAYER_NAME_LENGTH;
    player->position.x = mn_read_float(in + offset); offset += 4;
    player->position.y = mn_read_float(in + offset); offset += 4;
    player->position.z = mn_read_float(in + offset); offset += 4;
    player->rotation.x = mn_read_float(in + offset); offset += 4;
    player->rotation.y = mn_read_float(in + offset); offset += 4;
    player->rotation.z = mn_read_float(in + offset); offset += 4;
    player->rotation.w = mn_read_float(in + offset); offset += 4;
    player->velocity.x = mn_read_float(in + offset); offset += 4;
    player->velocity.y = mn_read_float(in + offset); offset += 4;
    player->velocity.z = mn_read_float(in + offset); offset += 4;
    return mn_player_is_valid(player);
}

static size_t mn_make_packet(uint8_t *out, uint8_t type, uint32_t app_id,
                             uint32_t room_id, const void *payload,
                             size_t payload_size) {
    size_t total = MN_HEADER_SIZE + payload_size;
    if (!out || total > MN_MAX_DATAGRAM_SIZE || payload_size > 65535u) return 0;
    mn_write_u32(out, MN_MAGIC);
    out[4] = MN_PROTOCOL_VERSION;
    out[5] = type;
    mn_write_u16(out + 6, (uint16_t)total);
    mn_write_u32(out + 8, app_id);
    mn_write_u32(out + 12, room_id);
    if (payload_size && payload) memcpy(out + MN_HEADER_SIZE, payload, payload_size);
    return total;
}

static void mn_send_raw_to(mn_instance *instance, const struct sockaddr_in *to,
                           const uint8_t *data, size_t size) {
    if (!instance || instance->socket == MN_BAD_SOCKET || !to || !data || size == 0)
        return;
#ifdef _WIN32
    (void)sendto(instance->socket, (const char *)data, (int)size, 0,
                 (const struct sockaddr *)to, (int)sizeof(*to));
#else
    (void)sendto(instance->socket, (const char *)data, size, 0,
                 (const struct sockaddr *)to, (socklen_t)sizeof(*to));
#endif
}

static void mn_send_to(mn_instance *instance, const struct sockaddr_in *to,
                       const uint8_t *data, size_t size) {
    uint8_t secure_packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t nonce[MN_SECURE_NONCE_BYTES];
    uint8_t aad[MN_HEADER_SIZE + 1u];
    size_t secure_size, cipher_size;
    uint8_t inner_type;
    if (!instance || !to || !data || size == 0) return;
    if (!instance->encryption_enabled) {
        mn_send_raw_to(instance, to, data, size);
        return;
    }
    if (size < MN_HEADER_SIZE || data[5] == MN_PKT_SECURE ||
        size + MN_SECURE_OVERHEAD + 1u > MN_MAX_DATAGRAM_SIZE ||
        instance->secure_nonce_counter == UINT32_MAX)
        return;

    inner_type = data[5];
    cipher_size = size - MN_HEADER_SIZE;
    instance->secure_nonce_counter++;
    mn_write_u64(nonce, instance->secure_nonce_prefix);
    mn_write_u32(nonce + 8u, instance->secure_nonce_counter);
    secure_size = mn_make_packet(secure_packet, MN_PKT_SECURE,
                                 mn_read_u32(data + 8u),
                                 mn_read_u32(data + 12u), NULL,
                                 MN_SECURE_OVERHEAD + 1u + cipher_size);
    if (secure_size == 0) return;
    memcpy(aad, secure_packet, MN_HEADER_SIZE);
    aad[MN_HEADER_SIZE] = inner_type;
    memcpy(secure_packet + MN_HEADER_SIZE, nonce, sizeof(nonce));
    secure_packet[MN_HEADER_SIZE + sizeof(nonce)] = inner_type;
    if (!mn_crypto_aes256gcm_encrypt(instance->room_key, nonce,
                                     aad, sizeof(aad),
                                     data + MN_HEADER_SIZE, cipher_size,
                                     secure_packet + MN_HEADER_SIZE + sizeof(nonce) + 1u,
                                     secure_packet + MN_HEADER_SIZE + sizeof(nonce) + 1u + cipher_size))
        return;
    mn_send_raw_to(instance, to, secure_packet, secure_size);
}

static int mn_accept_secure_nonce(mn_instance *instance,
                                  const struct sockaddr_in *from,
                                  const uint8_t nonce[MN_SECURE_NONCE_BYTES]) {
    uint64_t prefix = mn_read_u64(nonce);
    uint32_t counter = mn_read_u32(nonce + 8u);
    mn_secure_replay *slot = NULL;
    size_t i;
    if (counter == 0) return 0;
    for (i = 0; i < MN_SECURE_REPLAY_CAPACITY; ++i) {
        mn_secure_replay *entry = &instance->secure_replays[i];
        if (entry->active && entry->nonce_prefix == prefix &&
            mn_same_address(&entry->address, from)) {
            slot = entry;
            break;
        }
    }
    if (!slot) {
        for (i = 0; i < MN_SECURE_REPLAY_CAPACITY; ++i) {
            if (!instance->secure_replays[i].active) {
                slot = &instance->secure_replays[i];
                break;
            }
        }
        if (!slot) {
            slot = &instance->secure_replays[0];
            for (i = 1; i < MN_SECURE_REPLAY_CAPACITY; ++i) {
                if (instance->secure_replays[i].age < slot->age)
                    slot = &instance->secure_replays[i];
            }
        }
        memset(slot, 0, sizeof(*slot));
        slot->active = 1;
        slot->address = *from;
        slot->nonce_prefix = prefix;
        slot->highest_counter = counter;
        slot->seen_window = 1u;
        slot->age = ++instance->secure_replay_age;
        return 1;
    }
    if (counter > slot->highest_counter) {
        uint32_t shift = counter - slot->highest_counter;
        slot->seen_window = shift >= 64u ? 1u : (slot->seen_window << shift) | 1u;
        slot->highest_counter = counter;
    } else {
        uint32_t difference = slot->highest_counter - counter;
        uint64_t bit;
        if (difference >= 64u) return 0;
        bit = (uint64_t)1u << (unsigned)difference;
        if ((slot->seen_window & bit) != 0u) return 0;
        slot->seen_window |= bit;
    }
    slot->age = ++instance->secure_replay_age;
    return 1;
}

static size_t mn_active_player_count(const mn_instance *instance) {
    size_t count = instance->info.role == MN_ROLE_HOST ? 1u : 0u;
    size_t i;
    for (i = 0; i < MN_MAX_PLAYERS; ++i)
        if (instance->peers[i].active) ++count;
    return count;
}

static void mn_refresh_host_players(mn_instance *instance) {
    size_t i, count = 0;
    memset(instance->players, 0, sizeof(instance->players));
    if (instance->info.role == MN_ROLE_HOST) instance->players[count++] = instance->local_player;
    for (i = 0; i < MN_MAX_PLAYERS && count < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active) instance->players[count++] = instance->peers[i].player;
    }
    instance->info.player_count = (uint8_t)count;
}

static void mn_send_offer(mn_instance *instance, const struct sockaddr_in *to) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[MN_OFFER_SIZE];
    size_t size;
    payload[0] = instance->info.player_count;
    payload[1] = instance->info.max_players;
    memset(payload + 2, 0, MN_ROOM_NAME_LENGTH + MN_PLAYER_NAME_LENGTH);
    mn_copy_text((char *)(payload + 2), MN_ROOM_NAME_LENGTH, instance->info.room_name);
    mn_copy_text((char *)(payload + 2 + MN_ROOM_NAME_LENGTH),
                 MN_PLAYER_NAME_LENGTH, instance->info.local_name);
    size = mn_make_packet(packet, MN_PKT_OFFER, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, to, packet, size);
}



static void mn_send_welcome(mn_instance *instance, const mn_peer *peer) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[MN_WELCOME_SIZE];
    size_t size;
    mn_write_u32(payload, peer->id);
    mn_write_u64(payload + 4, peer->token);
    payload[12] = instance->info.max_players;
    memset(payload + 13, 0, MN_ROOM_NAME_LENGTH);
    mn_copy_text((char *)(payload + 13), MN_ROOM_NAME_LENGTH, instance->info.room_name);
    size = mn_make_packet(packet, MN_PKT_WELCOME, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &peer->address, packet, size);
}

static void mn_send_snapshot_to(mn_instance *instance,
                                const struct sockaddr_in *to) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[6u + MN_MAX_PLAYERS * MN_WIRE_PLAYER_SIZE];
    size_t i, count = 0, size;
    instance->snapshot_sequence++;
    mn_write_u32(payload, instance->snapshot_sequence);
    payload[5] = instance->info.match_started ? 1u : 0u;
    mn_encode_player(payload + 6u + count * MN_WIRE_PLAYER_SIZE,
                     &instance->local_player);
    ++count;
    for (i = 0; i < MN_MAX_PLAYERS && count < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active) {
            mn_encode_player(payload + 6u + count * MN_WIRE_PLAYER_SIZE,
                             &instance->peers[i].player);
            ++count;
        }
    }
    payload[4] = (uint8_t)count;
    size = mn_make_packet(packet, MN_PKT_SNAPSHOT, instance->config.app_id,
                          instance->info.room_id, payload,
                          6u + count * MN_WIRE_PLAYER_SIZE);
    mn_send_to(instance, to, packet, size);
}

static void mn_broadcast_snapshot(mn_instance *instance) {
    size_t i;
    mn_refresh_host_players(instance);
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active)
            mn_send_snapshot_to(instance, &instance->peers[i].address);
    }
}

static int mn_device_id_is_valid(const uint8_t device_id[MN_DEVICE_ID_BYTES]) {
    size_t i;
    uint8_t value = 0;
    if (!device_id) return 0;
    for (i = 0; i < MN_DEVICE_ID_BYTES; ++i) value |= device_id[i];
    return value != 0;
}

static int mn_secure_equal(const uint8_t *left, const uint8_t *right,
                            size_t size) {
    uint8_t difference = 0;
    size_t i;
    if (!left || !right) return 0;
    for (i = 0; i < size; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
    return difference == 0;
}

static int mn_device_secret_is_valid(
    const uint8_t device_secret[MN_DEVICE_SECRET_BYTES]) {
    size_t i;
    uint8_t value = 0;
    if (!device_secret) return 0;
    for (i = 0; i < MN_DEVICE_SECRET_BYTES; ++i) value |= device_secret[i];
    return value != 0;
}

static size_t mn_find_paired_device_index(
    const mn_instance *instance,
    const uint8_t device_id[MN_DEVICE_ID_BYTES]) {
    size_t i;
    for (i = 0; i < instance->paired_device_count; ++i) {
        if (memcmp(instance->paired_devices[i], device_id, MN_DEVICE_ID_BYTES) == 0)
            return i;
    }
    return MN_MAX_PAIRED_DEVICES;
}

static int mn_make_pairing_proof(
    const uint8_t device_secret[MN_DEVICE_SECRET_BYTES],
    const uint8_t device_id[MN_DEVICE_ID_BYTES],
    const uint8_t challenge_nonce[32],
    uint32_t room_id, uint32_t app_id, uint8_t proof[32]) {
    uint8_t input[56];
    if (!device_secret || !device_id || !challenge_nonce || !proof) return 0;
    memcpy(input, challenge_nonce, 32u);
    memcpy(input + 32u, device_id, MN_DEVICE_ID_BYTES);
    mn_write_u32(input + 48u, room_id);
    mn_write_u32(input + 52u, app_id);
    return mn_crypto_hmac_sha256(device_secret, MN_DEVICE_SECRET_BYTES,
                                 input, sizeof(input), proof);
}

static int mn_send_pairing_challenge(mn_instance *instance, mn_peer *peer) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[MN_DEVICE_ID_BYTES + 32u];
    size_t packet_size;
    if (!instance || !peer || !peer->pending_approval ||
        !instance->encryption_enabled ||
        !mn_device_secret_is_valid(peer->device_secret))
        return 0;
    if (!peer->challenge_pending) {
        if (!mn_random_bytes(peer->challenge_nonce, sizeof(peer->challenge_nonce)))
            return 0;
        peer->challenge_pending = 1;
    }
    memcpy(payload, peer->device_id, MN_DEVICE_ID_BYTES);
    memcpy(payload + MN_DEVICE_ID_BYTES, peer->challenge_nonce,
           sizeof(peer->challenge_nonce));
    packet_size = mn_make_packet(packet, MN_PKT_PAIRING_CHALLENGE,
                                 instance->config.app_id, instance->info.room_id,
                                 payload, sizeof(payload));
    if (packet_size == 0) return 0;
    mn_send_to(instance, &peer->address, packet, packet_size);
    return 1;
}

static void mn_emit_pairing_event(mn_instance *instance, mn_event_type type,
                                  const mn_peer *peer, const char *message) {
    mn_event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    if (peer) {
        event.player = peer->player;
        memcpy(event.device_id, peer->device_id, MN_DEVICE_ID_BYTES);
    }
    mn_copy_text(event.text, sizeof(event.text), message);
    mn_push_event(instance, &event);
}

static void mn_activate_peer(mn_instance *instance, mn_peer *peer) {
    mn_event event;
    if (!peer || (!peer->pending_approval && !peer->active)) return;
    if (!mn_random_u64(&peer->token)) {
        memset(peer, 0, sizeof(*peer));
        mn_set_error(instance, "Secure random number generation failed");
        return;
    }
    peer->id = instance->next_player_id++;
    if (peer->id == 0) peer->id = instance->next_player_id++;
    peer->pending_approval = 0;
    peer->challenge_pending = 0;
    peer->active = 1;
    peer->player.id = peer->id;
    peer->player.colour_rgba = 0xFFFFFFFFu;
    peer->player.rotation.w = 1.0f;
    mn_secure_zero(peer->device_secret, sizeof(peer->device_secret));
    mn_secure_zero(peer->challenge_nonce, sizeof(peer->challenge_nonce));
    mn_refresh_host_players(instance);
    memset(&event, 0, sizeof(event));
    event.type = MN_EVENT_PLAYER_JOINED;
    event.player_id = peer->id;
    event.player = peer->player;
    memcpy(event.device_id, peer->device_id, MN_DEVICE_ID_BYTES);
    mn_push_event(instance, &event);
    mn_send_welcome(instance, peer);
    mn_broadcast_snapshot(instance);
}

static void mn_remove_peer(mn_instance *instance, mn_peer *peer,
                           const char *reason) {
    uint32_t player_id;
    if (!peer || !peer->active) return;
    player_id = peer->id;
    mn_emit_simple_event(instance, MN_EVENT_PLAYER_LEFT, reason, player_id);
    {
        size_t i;
        for (i = 0; i < MN_MAX_RELIABLE_PENDING; ++i) {
            mn_reliable_entry *entry = &instance->reliable[i];
            if (entry->active && entry->peer_id == player_id) {
                mn_emit_reliable_status(instance, MN_EVENT_MESSAGE_FAILED, entry,
                                         player_id,
                                         "Remote player left before delivery was acknowledged");
                memset(entry, 0, sizeof(*entry));
            }
        }
    }
    memset(peer, 0, sizeof(*peer));
    mn_refresh_host_players(instance);
}

static void mn_add_room(mn_instance *instance, const struct sockaddr_in *from,
                        uint32_t room_id, const uint8_t *payload) {
    char ip[48];
    size_t i;
    mn_room room;
    if (!inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip))) return;
    if (payload[1] < 2u || payload[1] > MN_MAX_PLAYERS || payload[0] == 0u ||
        payload[0] > payload[1]) return;
    memset(&room, 0, sizeof(room));
    memcpy(room.name, payload + 2, MN_ROOM_NAME_LENGTH);
    room.name[MN_ROOM_NAME_LENGTH - 1] = '\0';
    memcpy(room.host_name, payload + 2 + MN_ROOM_NAME_LENGTH, MN_PLAYER_NAME_LENGTH);
    room.host_name[MN_PLAYER_NAME_LENGTH - 1] = '\0';
    mn_copy_text(room.address, sizeof(room.address), ip);
    room.port = ntohs(from->sin_port);
    room.player_count = payload[0];
    room.max_players = payload[1];
    room.room_id = room_id;
    room.app_id = instance->config.app_id;
    for (i = 0; i < instance->room_count; ++i) {
        if (instance->rooms[i].room_id == room_id &&
            strcmp(instance->rooms[i].address, ip) == 0) {
            instance->rooms[i] = room;
            return;
        }
    }
    if (instance->room_count >= MN_MAX_ROOMS) return;
    instance->rooms[instance->room_count++] = room;
    {
        mn_event event;
        memset(&event, 0, sizeof(event));
        event.type = MN_EVENT_ROOM_FOUND;
        mn_copy_text(event.text, sizeof(event.text), room.name);
        mn_push_event(instance, &event);
    }
}



static void mn_send_discover_to(mn_instance *instance,
                                  const struct sockaddr_in *destination) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    size_t size;
    if (!instance || !destination) return;
    size = mn_make_packet(packet, MN_PKT_DISCOVER, instance->config.app_id,
                          0, NULL, 0);
    mn_send_to(instance, destination, packet, size);
}

static void mn_send_discover(mn_instance *instance) {
    struct sockaddr_in destination;
    if (!instance || !instance->config.enable_broadcast) return;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(instance->config.port);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    mn_send_discover_to(instance, &destination);
}


static int mn_dns_write_name(uint8_t *out, size_t capacity, size_t *offset,
                             const char *name) {
    const char *cursor = name;
    if (!out || !offset || !name) return 0;
    while (*cursor) {
        const char *dot = strchr(cursor, '.');
        size_t label_size = dot ? (size_t)(dot - cursor) : strlen(cursor);
        if (label_size == 0) {
            cursor = dot ? dot + 1 : cursor + label_size;
            continue;
        }
        if (label_size > 63u || *offset + 1u + label_size >= capacity) return 0;
        out[(*offset)++] = (uint8_t)label_size;
        memcpy(out + *offset, cursor, label_size);
        *offset += label_size;
        if (!dot) break;
        cursor = dot + 1;
    }
    if (*offset >= capacity) return 0;
    out[(*offset)++] = 0;
    return 1;
}

static int mn_dns_read_name(const uint8_t *packet, size_t packet_size,
                            size_t *offset, char *out, size_t out_capacity) {
    size_t cursor, consumed = 0, used = 0;
    int jumped = 0;
    unsigned hops = 0;
    if (!packet || !offset || !out || out_capacity == 0 || *offset >= packet_size) return 0;
    cursor = *offset;
    out[0] = '\0';
    for (;;) {
        uint8_t length;
        if (cursor >= packet_size || ++hops > 32u) return 0;
        length = packet[cursor++];
        if ((length & 0xC0u) == 0xC0u) {
            uint16_t pointer;
            if (cursor >= packet_size) return 0;
            pointer = (uint16_t)(((uint16_t)(length & 0x3Fu) << 8) | packet[cursor++]);
            if (pointer >= packet_size) return 0;
            if (!jumped) consumed = cursor - *offset;
            cursor = pointer;
            jumped = 1;
            continue;
        }
        if ((length & 0xC0u) != 0) return 0;
        if (length == 0) {
            if (!jumped) consumed = cursor - *offset;
            *offset += consumed;
            return 1;
        }
        if ((size_t)length > packet_size - cursor) return 0;
        if (used && used + 1u >= out_capacity) return 0;
        if (used) out[used++] = '.';
        if (used + length >= out_capacity) return 0;
        {
            size_t i;
            for (i = 0; i < length; ++i)
                out[used++] = (char)packet[cursor + i];
        }
        out[used] = '\0';
        cursor += length;
    }
}

static int mn_dns_name_equal(const char *left, const char *right) {
    size_t i;
    if (!left || !right || strlen(left) != strlen(right)) return 0;
    for (i = 0; left[i]; ++i)
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) return 0;
    return 1;
}

static int mn_dns_append_rr(uint8_t *packet, size_t capacity, size_t *offset,
                            const char *owner, uint16_t type,
                            const uint8_t *rdata, size_t rdata_size) {
    if (!mn_dns_write_name(packet, capacity, offset, owner)) return 0;
    if (*offset + 10u + rdata_size > capacity || rdata_size > 65535u) return 0;
    mn_write_u16(packet + *offset, type); *offset += 2;
    mn_write_u16(packet + *offset, 1u); *offset += 2;
    mn_write_u32(packet + *offset, 120u); *offset += 4;
    mn_write_u16(packet + *offset, (uint16_t)rdata_size); *offset += 2;
    if (rdata_size) memcpy(packet + *offset, rdata, rdata_size);
    *offset += rdata_size;
    return 1;
}

static void mn_send_mdns_packet(mn_instance *instance,
                                const struct sockaddr_in *destination,
                                const uint8_t *packet, size_t packet_size) {
    if (!instance || instance->mdns_socket == MN_BAD_SOCKET ||
        !destination || !packet || packet_size == 0) return;
#ifdef _WIN32
    (void)sendto(instance->mdns_socket, (const char *)packet, (int)packet_size, 0,
                 (const struct sockaddr *)destination, (int)sizeof(*destination));
#else
    (void)sendto(instance->mdns_socket, (const char *)packet, packet_size, 0,
                 (const struct sockaddr *)destination, (socklen_t)sizeof(*destination));
#endif
}

static void mn_send_mdns_query(mn_instance *instance) {
    uint8_t packet[MN_MDNS_MAX_PACKET];
    size_t offset = 12u;
    struct sockaddr_in destination;
    mn_interface interfaces[64];
    size_t count, i, sent = 0;
    struct in_addr any_interface;
    if (!instance || instance->mdns_socket == MN_BAD_SOCKET) return;
    memset(packet, 0, sizeof(packet));
    mn_write_u16(packet + 4, 1u); /* QDCOUNT */
    if (!mn_dns_write_name(packet, sizeof(packet), &offset, "_multinet._udp.local")) return;
    if (offset + 4u > sizeof(packet)) return;
    mn_write_u16(packet + offset, 12u); offset += 2; /* PTR */
    mn_write_u16(packet + offset, 0x8001u); offset += 2; /* IN, unicast response requested */
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(MN_MDNS_PORT);
    destination.sin_addr.s_addr = inet_addr("224.0.0.251");

    count = mn_get_interfaces(interfaces, 64);
    for (i = 0; i < count && i < 64; ++i) {
        struct in_addr local_address;
        if (interfaces[i].type == MN_INTERFACE_LOOPBACK ||
            !interfaces[i].address[0] ||
            inet_pton(AF_INET, interfaces[i].address, &local_address) != 1)
            continue;
        if (setsockopt(instance->mdns_socket, IPPROTO_IP, IP_MULTICAST_IF,
                       (const char *)&local_address,
                       (mn_socklen)sizeof(local_address)) != 0)
            continue;
        mn_send_mdns_packet(instance, &destination, packet, offset);
        ++sent;
    }

    any_interface.s_addr = htonl(INADDR_ANY);
    (void)setsockopt(instance->mdns_socket, IPPROTO_IP, IP_MULTICAST_IF,
                     (const char *)&any_interface, (mn_socklen)sizeof(any_interface));
    if (sent == 0u)
        mn_send_mdns_packet(instance, &destination, packet, offset);
}


static void mn_send_auto_discover(mn_instance *instance) {
    mn_interface interfaces[64];
    size_t count, i;
    if (!instance) return;
    if (instance->config.enable_broadcast) {
        mn_send_discover(instance);
        count = mn_get_interfaces(interfaces, 64);
        for (i = 0; i < count && i < 64; ++i) {
            struct sockaddr_in destination;
            if (!interfaces[i].broadcast[0] ||
                interfaces[i].type == MN_INTERFACE_LOOPBACK ||
                strcmp(interfaces[i].broadcast, "255.255.255.255") == 0) continue;
            if (instance->bound_ipv4[0] &&
                strcmp(instance->bound_ipv4, interfaces[i].address) != 0) continue;
            memset(&destination, 0, sizeof(destination));
            destination.sin_family = AF_INET;
            destination.sin_port = htons(instance->config.port);
            if (inet_pton(AF_INET, interfaces[i].broadcast, &destination.sin_addr) != 1)
                continue;
            mn_send_discover_to(instance, &destination);
        }
    }
    mn_send_mdns_query(instance);
}

static unsigned mn_mdns_subnet_score(const mn_instance *instance,
                                    const char *candidate_ipv4) {
    mn_interface interfaces[64];
    struct in_addr candidate;
    uint32_t candidate_host;
    size_t count, i;
    unsigned best_score = 0;
    if (!instance || !candidate_ipv4 ||
        inet_pton(AF_INET, candidate_ipv4, &candidate) != 1)
        return 0;
    candidate_host = ntohl(candidate.s_addr);
    count = mn_get_interfaces(interfaces, 64);
    for (i = 0; i < count && i < 64; ++i) {
        struct in_addr local_address, local_mask;
        uint32_t address_host, mask_host, bits;
        unsigned prefix = 0;
        if (interfaces[i].type == MN_INTERFACE_LOOPBACK ||
            inet_pton(AF_INET, interfaces[i].address, &local_address) != 1 ||
            inet_pton(AF_INET, interfaces[i].netmask, &local_mask) != 1)
            continue;
        address_host = ntohl(local_address.s_addr);
        mask_host = ntohl(local_mask.s_addr);
        if ((address_host & mask_host) != (candidate_host & mask_host))
            continue;
        if (address_host == candidate_host) return 33u;
        bits = mask_host;
        while (bits & 0x80000000u) {
            ++prefix;
            bits <<= 1;
        }
        if (prefix > best_score) best_score = prefix;
    }
    return best_score;
}

static void mn_send_mdns_announcement(mn_instance *instance,
                                      const struct sockaddr_in *destination) {
    uint8_t packet[MN_MDNS_MAX_PACKET];
    uint8_t rdata[512];
    char instance_name[96], host_target[96], item[128];
    size_t offset = 12u, rd_offset, packet_size;
    size_t interface_count, interface_index, address_count = 0;
    mn_interface interfaces[64];
    struct in_addr ipv4;
    if (!instance || !destination || instance->info.role != MN_ROLE_HOST) return;
    memset(packet, 0, sizeof(packet));
    mn_write_u16(packet + 2, 0x8400u); /* response + authoritative */
    mn_write_u16(packet + 6, 1u); /* ANCOUNT: PTR */
    mn_write_u16(packet + 10, 2u); /* SRV and TXT; A records are appended below. */
    (void)snprintf(instance_name, sizeof(instance_name), "mn-%u._multinet._udp.local",
                   (unsigned)instance->info.room_id);
    (void)snprintf(host_target, sizeof(host_target), "multinet-%u.local",
                   (unsigned)instance->info.room_id);

    /* PTR answer points to the advertised service instance. */
    if (!mn_dns_write_name(packet, sizeof(packet), &offset, "_multinet._udp.local")) return;
    mn_write_u16(packet + offset, 12u); offset += 2;
    mn_write_u16(packet + offset, 1u); offset += 2;
    mn_write_u32(packet + offset, 120u); offset += 4;
    rd_offset = offset + 2u;
    if (!mn_dns_write_name(packet, sizeof(packet), &rd_offset, instance_name)) return;
    mn_write_u16(packet + offset, (uint16_t)(rd_offset - offset - 2u)); offset += 2;
    offset = rd_offset;

    /* SRV record points to the host's IPv4 A record. */
    rd_offset = 0;
    mn_write_u16(rdata + rd_offset, 0u); rd_offset += 2;
    mn_write_u16(rdata + rd_offset, 0u); rd_offset += 2;
    mn_write_u16(rdata + rd_offset, instance->config.port); rd_offset += 2;
    if (!mn_dns_write_name(rdata, sizeof(rdata), &rd_offset, host_target)) return;
    if (!mn_dns_append_rr(packet, sizeof(packet), &offset, instance_name,
                          33u, rdata, rd_offset)) return;

    /* TXT metadata makes the result usable without parsing the SRV instance label. */
    rd_offset = 0;
#define MN_MDNS_TXT(KEY, VALUE) do { \
    (void)snprintf(item, sizeof(item), "%s=%s", KEY, VALUE); \
    { size_t text_length = strlen(item); \
      if (text_length > 120u || rd_offset + text_length + 1u > sizeof(rdata)) return; \
      rdata[rd_offset++] = (uint8_t)text_length; \
      memcpy(rdata + rd_offset, item, text_length); rd_offset += text_length; } \
} while (0)
    {
        char number[24];
        MN_MDNS_TXT("room", instance->info.room_name);
        MN_MDNS_TXT("host", instance->info.local_name);
        (void)snprintf(number, sizeof(number), "%u", (unsigned)instance->config.app_id);
        MN_MDNS_TXT("appid", number);
        (void)snprintf(number, sizeof(number), "%u", (unsigned)instance->info.room_id);
        MN_MDNS_TXT("roomid", number);
        (void)snprintf(number, sizeof(number), "%u", (unsigned)instance->info.player_count);
        MN_MDNS_TXT("players", number);
        (void)snprintf(number, sizeof(number), "%u", (unsigned)instance->info.max_players);
        MN_MDNS_TXT("max", number);
    }
#undef MN_MDNS_TXT
    if (!mn_dns_append_rr(packet, sizeof(packet), &offset, instance_name,
                          16u, rdata, rd_offset)) return;

    /* Advertise all host IPv4 addresses. A client can choose the address
       that belongs to its local Wi-Fi, Ethernet, Wi-Fi Direct or PAN subnet. */
    if (instance->bound_ipv4[0] &&
        strcmp(instance->bound_ipv4, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, instance->bound_ipv4, &ipv4) != 1) return;
        if (!mn_dns_append_rr(packet, sizeof(packet), &offset, host_target,
                              1u, (const uint8_t *)&ipv4.s_addr, 4u)) return;
        address_count = 1u;
    } else {
        interface_count = mn_get_interfaces(interfaces, 64);
        for (interface_index = 0;
             interface_index < interface_count && interface_index < 64;
             ++interface_index) {
            if (interfaces[interface_index].type == MN_INTERFACE_LOOPBACK ||
                !interfaces[interface_index].address[0] ||
                inet_pton(AF_INET, interfaces[interface_index].address, &ipv4) != 1)
                continue;
            if (!mn_dns_append_rr(packet, sizeof(packet), &offset, host_target,
                                  1u, (const uint8_t *)&ipv4.s_addr, 4u))
                break;
            ++address_count;
        }
    }
    if (address_count == 0u) {
        if (inet_pton(AF_INET, "127.0.0.1", &ipv4) != 1 ||
            !mn_dns_append_rr(packet, sizeof(packet), &offset, host_target,
                              1u, (const uint8_t *)&ipv4.s_addr, 4u))
            return;
        address_count = 1u;
    }
    mn_write_u16(packet + 10, (uint16_t)(2u + address_count));
    packet_size = offset;
    mn_send_mdns_packet(instance, destination, packet, packet_size);
}

static void mn_mdns_add_room(mn_instance *instance, const char *ip,
                             uint16_t port, const char *room_name,
                             const char *host_name, uint32_t app_id,
                             uint32_t room_id, uint8_t players,
                             uint8_t max_players) {
    mn_room room;
    size_t i;
    if (!instance || !ip || !ip[0] || port == 0 || app_id != instance->config.app_id ||
        room_id == 0 || players == 0 || max_players < 2 ||
        max_players > MN_MAX_PLAYERS || players > max_players) return;
    memset(&room, 0, sizeof(room));
    mn_copy_text(room.address, sizeof(room.address), ip);
    mn_copy_text(room.name, sizeof(room.name), room_name);
    mn_copy_text(room.host_name, sizeof(room.host_name), host_name);
    room.port = port;
    room.app_id = app_id;
    room.room_id = room_id;
    room.player_count = players;
    room.max_players = max_players;
    for (i = 0; i < instance->room_count; ++i) {
        if (instance->rooms[i].room_id == room_id &&
            strcmp(instance->rooms[i].address, ip) == 0) {
            instance->rooms[i] = room;
            return;
        }
    }
    if (instance->room_count >= MN_MAX_ROOMS) return;
    instance->rooms[instance->room_count++] = room;
    {
        mn_event event;
        memset(&event, 0, sizeof(event));
        event.type = MN_EVENT_ROOM_FOUND;
        mn_copy_text(event.text, sizeof(event.text), room.name);
        mn_push_event(instance, &event);
    }
}

static void mn_handle_mdns_query(mn_instance *instance,
                                 const uint8_t *packet, size_t packet_size,
                                 const struct sockaddr_in *from) {
    uint16_t flags, questions;
    size_t offset = 12u;
    uint16_t q;
    if (packet_size < 12u || instance->info.role != MN_ROLE_HOST ||
        instance->info.status != MN_STATUS_HOSTING) return;
    flags = mn_read_u16(packet + 2);
    if (flags & 0x8000u) return; /* response */
    questions = mn_read_u16(packet + 4);
    for (q = 0; q < questions; ++q) {
        char name[256];
        uint16_t type, klass;
        if (!mn_dns_read_name(packet, packet_size, &offset, name, sizeof(name)) ||
            offset + 4u > packet_size) return;
        type = mn_read_u16(packet + offset); offset += 2;
        klass = (uint16_t)(mn_read_u16(packet + offset) & 0x7FFFu); offset += 2;
        if (klass == 1u && (type == 12u || type == 255u) &&
            mn_dns_name_equal(name, "_multinet._udp.local")) {
            struct sockaddr_in reply = *from;
            reply.sin_port = htons(MN_MDNS_PORT);
            mn_send_mdns_announcement(instance, &reply);
            return;
        }
    }
}

static void mn_handle_mdns_response(mn_instance *instance,
                                    const uint8_t *packet, size_t packet_size) {
    uint16_t flags, questions, answers, authorities, additional;
    uint32_t app_id = 0, room_id = 0;
    uint16_t service_port = 0;
    uint8_t player_count = 0, max_players = 0;
    char room_name[MN_ROOM_NAME_LENGTH] = "";
    char host_name[MN_PLAYER_NAME_LENGTH] = "";
    char ptr_instance_name[256] = "", srv_owner[256] = "", txt_owner[256] = "";
    char target_name[256] = "", address_name[256] = "", ip[48] = "";
    int found_service_ptr = 0;
    unsigned best_address_score = 0;
    size_t offset = 12u;
    uint32_t record_count, r;
    if (!instance || packet_size < 12u) return;
    flags = mn_read_u16(packet + 2);
    if (!(flags & 0x8000u)) return;
    questions = mn_read_u16(packet + 4);
    answers = mn_read_u16(packet + 6);
    authorities = mn_read_u16(packet + 8);
    additional = mn_read_u16(packet + 10);
    for (r = 0; r < questions; ++r) {
        char question_name[256];
        if (!mn_dns_read_name(packet, packet_size, &offset, question_name,
                              sizeof(question_name)) || offset + 4u > packet_size) return;
        offset += 4u;
    }
    record_count = (uint32_t)answers + authorities + additional;
    for (r = 0; r < record_count; ++r) {
        char owner[256];
        uint16_t type, klass, rdlength;
        size_t data_offset, end_offset;
        if (!mn_dns_read_name(packet, packet_size, &offset, owner, sizeof(owner)) ||
            offset + 10u > packet_size) return;
        type = mn_read_u16(packet + offset); offset += 2;
        klass = (uint16_t)(mn_read_u16(packet + offset) & 0x7FFFu); offset += 2;
        offset += 4u; /* TTL */
        rdlength = mn_read_u16(packet + offset); offset += 2;
        data_offset = offset;
        end_offset = data_offset + rdlength;
        if (end_offset > packet_size) return;
        if (klass == 1u && type == 12u &&
            mn_dns_name_equal(owner, "_multinet._udp.local")) {
            size_t name_offset = data_offset;
            if (!mn_dns_read_name(packet, packet_size, &name_offset,
                                  ptr_instance_name, sizeof(ptr_instance_name))) return;
            found_service_ptr = 1;
        } else if (klass == 1u && type == 33u && rdlength >= 7u) {
            size_t name_offset = data_offset + 6u;
            mn_copy_text(srv_owner, sizeof(srv_owner), owner);
            service_port = mn_read_u16(packet + data_offset + 4u);
            if (!mn_dns_read_name(packet, packet_size, &name_offset,
                                  target_name, sizeof(target_name))) return;
        } else if (klass == 1u && type == 16u) {
            size_t cursor = data_offset;
            mn_copy_text(txt_owner, sizeof(txt_owner), owner);
            while (cursor < end_offset) {
                uint8_t text_length = packet[cursor++];
                char field[160];
                char *equals;
                if ((size_t)text_length > end_offset - cursor) break;
                if (text_length >= sizeof(field)) { cursor += text_length; continue; }
                memcpy(field, packet + cursor, text_length);
                field[text_length] = '\0';
                cursor += text_length;
                equals = strchr(field, '=');
                if (!equals) continue;
                *equals++ = '\0';
                if (strcmp(field, "room") == 0) mn_copy_text(room_name, sizeof(room_name), equals);
                else if (strcmp(field, "host") == 0) mn_copy_text(host_name, sizeof(host_name), equals);
                else if (strcmp(field, "appid") == 0) app_id = (uint32_t)strtoul(equals, NULL, 10);
                else if (strcmp(field, "roomid") == 0) room_id = (uint32_t)strtoul(equals, NULL, 10);
                else if (strcmp(field, "players") == 0) player_count = (uint8_t)strtoul(equals, NULL, 10);
                else if (strcmp(field, "max") == 0) max_players = (uint8_t)strtoul(equals, NULL, 10);
            }
        } else if (klass == 1u && type == 1u && rdlength == 4u) {
            struct in_addr ipv4;
            if (target_name[0] && mn_dns_name_equal(owner, target_name)) {
                char candidate[48];
                unsigned candidate_score;
                memcpy(&ipv4.s_addr, packet + data_offset, 4u);
                if (!inet_ntop(AF_INET, &ipv4, candidate, sizeof(candidate)))
                    candidate[0] = '\0';
                candidate_score = candidate[0] ?
                    mn_mdns_subnet_score(instance, candidate) : 0u;
                if (candidate[0] && (!ip[0] ||
                    candidate_score > best_address_score)) {
                    mn_copy_text(ip, sizeof(ip), candidate);
                    mn_copy_text(address_name, sizeof(address_name), owner);
                    best_address_score = candidate_score;
                }
            }
        }
        offset = end_offset;
    }
    if (found_service_ptr && ptr_instance_name[0] && srv_owner[0] && txt_owner[0] &&
        mn_dns_name_equal(ptr_instance_name, srv_owner) &&
        mn_dns_name_equal(ptr_instance_name, txt_owner) &&
        target_name[0] && address_name[0] &&
        mn_dns_name_equal(target_name, address_name) &&
        ip[0] && room_name[0]) {
        mn_mdns_add_room(instance, ip, service_port, room_name, host_name,
                         app_id, room_id, player_count, max_players);
    }
}

static void mn_send_join(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[MN_DEVICE_ID_BYTES + MN_DEVICE_SECRET_BYTES +
                    MN_PLAYER_NAME_LENGTH];
    size_t size;
    memset(payload, 0, sizeof(payload));
    memcpy(payload, instance->device_id, MN_DEVICE_ID_BYTES);
    if (instance->encryption_enabled)
        memcpy(payload + MN_DEVICE_ID_BYTES, instance->device_secret,
               MN_DEVICE_SECRET_BYTES);
    mn_copy_text((char *)(payload + MN_DEVICE_ID_BYTES + MN_DEVICE_SECRET_BYTES),
                 MN_PLAYER_NAME_LENGTH, instance->info.local_name);
    size = mn_make_packet(packet, MN_PKT_JOIN, instance->config.app_id,
                          0, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, size);
}

static void mn_send_state(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[12u + MN_WIRE_PLAYER_SIZE];
    size_t size;
    mn_write_u64(payload, instance->session_token);
    instance->next_state_sequence++;
    mn_write_u32(payload + 8, instance->next_state_sequence);
    mn_encode_player(payload + 12, &instance->local_player);
    size = mn_make_packet(packet, MN_PKT_STATE, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, size);
}

static void mn_send_leave(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[8];
    size_t size;
    mn_write_u64(payload, instance->session_token);
    size = mn_make_packet(packet, MN_PKT_LEAVE, instance->config.app_id,
                          instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, size);
}

static void mn_emit_message(mn_instance *instance, uint32_t sender_id,
                            uint16_t channel, const uint8_t *data, size_t size) {
    mn_event event;
    if (size > MN_MAX_MESSAGE_BYTES) return;
    memset(&event, 0, sizeof(event));
    event.type = MN_EVENT_MESSAGE;
    event.player_id = sender_id;
    event.channel = channel;
    event.data_size = (uint16_t)size;
    if (size) memcpy(event.data, data, size);
    mn_push_event(instance, &event);
}

static void mn_forward_message(mn_instance *instance, uint32_t sender_id,
                               uint32_t target_id, uint16_t channel,
                               const uint8_t *data, size_t size) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[12u + MN_MAX_MESSAGE_BYTES];
    size_t i, packet_size;
    mn_write_u32(payload, sender_id);
    mn_write_u32(payload + 4, target_id);
    mn_write_u16(payload + 8, channel);
    mn_write_u16(payload + 10, (uint16_t)size);
    if (size) memcpy(payload + 12, data, size);
    packet_size = mn_make_packet(packet, MN_PKT_MESSAGE,
                                 instance->config.app_id, instance->info.room_id,
                                 payload, 12u + size);
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        mn_peer *peer = &instance->peers[i];
        if (!peer->active || peer->id == sender_id) continue;
        if (target_id != 0 && target_id != peer->id) continue;
        mn_send_to(instance, &peer->address, packet, packet_size);
    }
}

static void mn_handle_host_message(mn_instance *instance, const uint8_t *payload,
                                   size_t payload_size,
                                   const struct sockaddr_in *from) {
    uint64_t token;
    uint32_t sender_id, target_id;
    uint16_t channel, data_size;
    mn_peer *peer;
    if (payload_size < 20u) return;
    token = mn_read_u64(payload);
    sender_id = mn_read_u32(payload + 8);
    target_id = mn_read_u32(payload + 12);
    channel = mn_read_u16(payload + 16);
    data_size = mn_read_u16(payload + 18);
    if ((size_t)data_size > MN_MAX_MESSAGE_BYTES || payload_size != 20u + data_size)
        return;
    peer = mn_find_peer_token(instance, token);
    if (!peer || !mn_same_address(&peer->address, from) || sender_id != peer->id) return;
    if (peer->message_cooldown > 0.0f) return;
    peer->message_cooldown = 0.08f;
    peer->silence = 0.0f;
    if (target_id != 0 && target_id != instance->info.local_player_id &&
        !mn_find_peer_id(instance, target_id)) return;
    mn_emit_message(instance, peer->id, channel, payload + 20, data_size);
    mn_forward_message(instance, peer->id, target_id, channel,
                        payload + 20, data_size);
}



static void mn_handle_client_message(mn_instance *instance, const uint8_t *payload,
                                     size_t payload_size) {
    uint32_t sender_id, target_id;
    uint16_t channel, data_size;
    if (payload_size < 12u) return;
    sender_id = mn_read_u32(payload);
    target_id = mn_read_u32(payload + 4);
    channel = mn_read_u16(payload + 8);
    data_size = mn_read_u16(payload + 10);
    if ((size_t)data_size > MN_MAX_MESSAGE_BYTES || payload_size != 12u + data_size)
        return;
    if (target_id != 0 && target_id != instance->info.local_player_id) return;
    mn_emit_message(instance, sender_id, channel, payload + 12, data_size);
}


static mn_reliable_entry *mn_find_reliable_slot(mn_instance *instance) {
    size_t i;
    for (i = 0; i < MN_MAX_RELIABLE_PENDING; ++i)
        if (!instance->reliable[i].active) return &instance->reliable[i];
    return NULL;
}

static int mn_reliable_was_seen(mn_instance *instance, uint32_t sender_id,
                                uint32_t message_id, int remember) {
    mn_seen_reliable *slot = NULL;
    mn_seen_reliable *free_slot = NULL;
    mn_seen_reliable *oldest = NULL;
    size_t i;
    if (!instance || sender_id == 0 || message_id == 0) return 1;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        mn_seen_reliable *candidate = &instance->reliable_seen[i];
        if (candidate->sender_id == sender_id) {
            slot = candidate;
            break;
        }
        if (candidate->sender_id == 0 && !free_slot) free_slot = candidate;
        if (candidate->sender_id != 0 &&
            (!oldest || candidate->age < oldest->age)) oldest = candidate;
    }
    if (!slot) {
        if (!remember) return 0;
        slot = free_slot ? free_slot : oldest;
        if (!slot) return 1;
        memset(slot, 0, sizeof(*slot));
        slot->sender_id = sender_id;
    }
    for (i = 0; i < slot->count; ++i) {
        if (slot->message_ids[i] == message_id) {
            instance->reliable_seen_age++;
            slot->age = instance->reliable_seen_age;
            return 1;
        }
    }
    if (!remember) return 0;
    slot->message_ids[slot->next] = message_id;
    slot->next = (uint8_t)((slot->next + 1u) % MN_RELIABLE_SEEN_IDS);
    if (slot->count < MN_RELIABLE_SEEN_IDS) slot->count++;
    instance->reliable_seen_age++;
    slot->age = instance->reliable_seen_age;
    return 0;
}

static void mn_send_reliable_entry(mn_instance *instance,
                                   mn_reliable_entry *entry) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[24u + MN_MAX_MESSAGE_BYTES];
    size_t payload_size, packet_size;
    if (!instance || !entry || !entry->active) return;
    if (instance->info.role == MN_ROLE_HOST) {
        mn_peer *peer = mn_find_peer_id(instance, entry->peer_id);
        if (!peer) return;
        mn_write_u32(payload, entry->sender_id);
        mn_write_u32(payload + 4, entry->target_id);
        mn_write_u32(payload + 8, entry->message_id);
        mn_write_u16(payload + 12, entry->channel);
        mn_write_u16(payload + 14, entry->data_size);
        if (entry->data_size) memcpy(payload + 16, entry->data, entry->data_size);
        payload_size = 16u + entry->data_size;
        packet_size = mn_make_packet(packet, MN_PKT_RELIABLE, instance->config.app_id,
                                     instance->info.room_id, payload, payload_size);
        mn_send_to(instance, &peer->address, packet, packet_size);
    } else if (instance->info.role == MN_ROLE_CLIENT &&
               instance->info.status == MN_STATUS_CONNECTED) {
        mn_write_u64(payload, instance->session_token);
        mn_write_u32(payload + 8, entry->sender_id);
        mn_write_u32(payload + 12, entry->target_id);
        mn_write_u32(payload + 16, entry->message_id);
        mn_write_u16(payload + 20, entry->channel);
        mn_write_u16(payload + 22, entry->data_size);
        if (entry->data_size) memcpy(payload + 24, entry->data, entry->data_size);
        payload_size = 24u + entry->data_size;
        packet_size = mn_make_packet(packet, MN_PKT_RELIABLE, instance->config.app_id,
                                     instance->info.room_id, payload, payload_size);
        mn_send_to(instance, &instance->server, packet, packet_size);
    }
}

static int mn_queue_reliable_for_peer(mn_instance *instance, uint32_t peer_id,
                                      uint32_t sender_id, uint32_t target_id,
                                      uint32_t message_id, uint16_t channel,
                                      const void *data, size_t size) {
    mn_reliable_entry *entry = mn_find_reliable_slot(instance);
    if (!entry) return 0;
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->peer_id = peer_id;
    entry->sender_id = sender_id;
    entry->target_id = target_id;
    entry->message_id = message_id;
    entry->channel = channel;
    entry->data_size = (uint16_t)size;
    if (size) memcpy(entry->data, data, size);
    entry->attempts = 1;
    mn_send_reliable_entry(instance, entry);
    return 1;
}

static void mn_send_reliable_ack_to_peer(mn_instance *instance,
                                         const struct sockaddr_in *address,
                                         uint32_t message_id) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[4];
    size_t packet_size;
    mn_write_u32(payload, message_id);
    packet_size = mn_make_packet(packet, MN_PKT_RELIABLE_ACK, instance->config.app_id,
                                 instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, address, packet, packet_size);
}

static void mn_send_reliable_ack_to_host(mn_instance *instance, uint32_t message_id) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[12];
    size_t packet_size;
    mn_write_u64(payload, instance->session_token);
    mn_write_u32(payload + 8, message_id);
    packet_size = mn_make_packet(packet, MN_PKT_RELIABLE_ACK, instance->config.app_id,
                                 instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, packet_size);
}

static void mn_emit_reliable_status(mn_instance *instance, mn_event_type type,
                                    const mn_reliable_entry *entry,
                                    uint32_t peer_id, const char *message) {
    mn_event event;
    if (!instance || !entry) return;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.player_id = peer_id;
    event.channel = entry->channel;
    event.delivery_id = entry->message_id;
    mn_copy_text(event.text, sizeof(event.text), message);
    mn_push_event(instance, &event);
}

static void mn_remove_reliable_entry(mn_instance *instance, uint32_t peer_id,
                                     uint32_t message_id) {
    size_t i;
    for (i = 0; i < MN_MAX_RELIABLE_PENDING; ++i) {
        mn_reliable_entry *entry = &instance->reliable[i];
        if (entry->active && entry->peer_id == peer_id &&
            entry->message_id == message_id) {
            mn_emit_reliable_status(instance, MN_EVENT_MESSAGE_ACKNOWLEDGED,
                                     entry, peer_id,
                                     "Reliable delivery acknowledged");
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void mn_handle_host_reliable(mn_instance *instance, const uint8_t *payload,
                                    size_t payload_size,
                                    const struct sockaddr_in *from) {
    uint64_t token;
    uint32_t sender_id, target_id, message_id;
    uint16_t channel, data_size;
    mn_peer *peer;
    size_t i, needed = 0, free_slots = 0;
    if (payload_size < 24u) return;
    token = mn_read_u64(payload);
    sender_id = mn_read_u32(payload + 8);
    target_id = mn_read_u32(payload + 12);
    message_id = mn_read_u32(payload + 16);
    channel = mn_read_u16(payload + 20);
    data_size = mn_read_u16(payload + 22);
    if (message_id == 0 || (size_t)data_size > MN_MAX_MESSAGE_BYTES ||
        payload_size != 24u + data_size) return;
    peer = mn_find_peer_token(instance, token);
    if (!peer || !mn_same_address(&peer->address, from) || sender_id != peer->id) return;
    if (target_id != 0) return;
    if (mn_reliable_was_seen(instance, peer->id, message_id, 0)) {
        /* An ACK may have been lost. Always re-ACK known deliveries, even
           while the peer is inside the ordinary message rate-limit window. */
        mn_send_reliable_ack_to_peer(instance, from, message_id);
        peer->silence = 0.0f;
        return;
    }
    if (peer->message_cooldown > 0.0f) return;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active && instance->peers[i].id != peer->id) needed++;
    }
    for (i = 0; i < MN_MAX_RELIABLE_PENDING; ++i)
        if (!instance->reliable[i].active) free_slots++;
    if (needed > free_slots) return;

    /* Only remember the ID after capacity is available, otherwise a retry
       could be ACKed even though this server never queued its fan-out. */
    (void)mn_reliable_was_seen(instance, peer->id, message_id, 1);
    peer->message_cooldown = 0.08f;
    peer->silence = 0.0f;
    mn_send_reliable_ack_to_peer(instance, from, message_id);
    mn_emit_message(instance, peer->id, channel, payload + 24, data_size);
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        mn_peer *recipient = &instance->peers[i];
        if (!recipient->active || recipient->id == peer->id) continue;
        (void)mn_queue_reliable_for_peer(instance, recipient->id, peer->id, 0,
                                         message_id, channel, payload + 24, data_size);
    }
}

static void mn_handle_client_reliable(mn_instance *instance, const uint8_t *payload,
                                      size_t payload_size) {
    uint32_t sender_id, target_id, message_id;
    uint16_t channel, data_size;
    if (payload_size < 16u) return;
    sender_id = mn_read_u32(payload);
    target_id = mn_read_u32(payload + 4);
    message_id = mn_read_u32(payload + 8);
    channel = mn_read_u16(payload + 12);
    data_size = mn_read_u16(payload + 14);
    if (message_id == 0 || (size_t)data_size > MN_MAX_MESSAGE_BYTES ||
        payload_size != 16u + data_size) return;
    if (target_id != 0 && target_id != instance->info.local_player_id) return;
    mn_send_reliable_ack_to_host(instance, message_id);
    if (mn_reliable_was_seen(instance, sender_id, message_id, 1)) return;
    mn_emit_message(instance, sender_id, channel, payload + 16, data_size);
}

static void mn_handle_host_reliable_ack(mn_instance *instance,
                                        const uint8_t *payload, size_t payload_size,
                                        const struct sockaddr_in *from) {
    uint64_t token;
    uint32_t message_id;
    mn_peer *peer;
    if (payload_size != 12u) return;
    token = mn_read_u64(payload);
    message_id = mn_read_u32(payload + 8);
    peer = mn_find_peer_token(instance, token);
    if (!peer || !mn_same_address(&peer->address, from)) return;
    peer->silence = 0.0f;
    mn_remove_reliable_entry(instance, peer->id, message_id);
}

static void mn_handle_client_reliable_ack(mn_instance *instance,
                                          const uint8_t *payload, size_t payload_size) {
    if (payload_size != 4u) return;
    mn_remove_reliable_entry(instance, 0, mn_read_u32(payload));
}


static void mn_send_pong_to_peer(mn_instance *instance,
                                 const struct sockaddr_in *address,
                                 uint32_t ping_id) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[4];
    size_t packet_size;
    mn_write_u32(payload, ping_id);
    packet_size = mn_make_packet(packet, MN_PKT_PONG, instance->config.app_id,
                                 instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, address, packet, packet_size);
}

static void mn_send_pong_to_host(mn_instance *instance, uint32_t ping_id) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[12];
    size_t packet_size;
    mn_write_u64(payload, instance->session_token);
    mn_write_u32(payload + 8, ping_id);
    packet_size = mn_make_packet(packet, MN_PKT_PONG, instance->config.app_id,
                                 instance->info.room_id, payload, sizeof(payload));
    mn_send_to(instance, &instance->server, packet, packet_size);
}

static void mn_complete_ping(mn_instance *instance) {
    instance->info.ping_ms = instance->ping_elapsed * 1000.0f;
    instance->info.ping_available = 1;
    instance->info.ping_pending = 0;
    instance->ping_elapsed = 0.0f;
    instance->ping_pending_id = 0;
    instance->ping_pending_target = 0;
}

static void mn_handle_host_ping(mn_instance *instance, const uint8_t *payload,
                                size_t payload_size, const struct sockaddr_in *from) {
    uint64_t token;
    uint32_t ping_id;
    mn_peer *peer;
    if (payload_size != 12u) return;
    token = mn_read_u64(payload);
    ping_id = mn_read_u32(payload + 8);
    if (ping_id == 0) return;
    peer = mn_find_peer_token(instance, token);
    if (!peer || !mn_same_address(&peer->address, from)) return;
    peer->silence = 0.0f;
    mn_send_pong_to_peer(instance, from, ping_id);
}

static void mn_handle_host_pong(mn_instance *instance, const uint8_t *payload,
                                size_t payload_size, const struct sockaddr_in *from) {
    uint64_t token;
    uint32_t ping_id;
    mn_peer *peer;
    if (payload_size != 12u) return;
    token = mn_read_u64(payload);
    ping_id = mn_read_u32(payload + 8);
    peer = mn_find_peer_token(instance, token);
    if (!peer || !mn_same_address(&peer->address, from)) return;
    if (!instance->info.ping_pending ||
        instance->ping_pending_target != peer->id ||
        instance->ping_pending_id != ping_id) return;
    peer->silence = 0.0f;
    mn_complete_ping(instance);
}

static void mn_handle_client_ping(mn_instance *instance,
                                  const uint8_t *payload, size_t payload_size) {
    if (payload_size != 4u) return;
    mn_send_pong_to_host(instance, mn_read_u32(payload));
}

static void mn_handle_client_pong(mn_instance *instance,
                                  const uint8_t *payload, size_t payload_size) {
    uint32_t ping_id;
    if (payload_size != 4u) return;
    ping_id = mn_read_u32(payload);
    if (!instance->info.ping_pending || instance->ping_pending_target != 0 ||
        instance->ping_pending_id != ping_id) return;
    mn_complete_ping(instance);
}

static void mn_handle_packet_internal(mn_instance *instance, const uint8_t *packet,
                                      size_t packet_size,
                                      const struct sockaddr_in *from,
                                      int authenticated) {
    uint8_t type;
    uint32_t app_id, room_id;
    const uint8_t *payload;
    size_t payload_size;
    if (packet_size < MN_HEADER_SIZE) return;
    if (mn_read_u32(packet) != MN_MAGIC || packet[4] != MN_PROTOCOL_VERSION ||
        mn_read_u16(packet + 6) != packet_size) return;
    type = packet[5];
    app_id = mn_read_u32(packet + 8);
    room_id = mn_read_u32(packet + 12);
    if (app_id != instance->config.app_id) return;
    payload = packet + MN_HEADER_SIZE;
    payload_size = packet_size - MN_HEADER_SIZE;

    if (type == MN_PKT_SECURE) {
        uint8_t plaintext[MN_MAX_DATAGRAM_SIZE];
        uint8_t aad[MN_HEADER_SIZE + 1u];
        uint8_t inner_type;
        size_t ciphertext_size, inner_size;
        if (!instance->encryption_enabled ||
            payload_size < MN_SECURE_OVERHEAD + 1u) return;
        inner_type = payload[MN_SECURE_NONCE_BYTES];
        if (inner_type == MN_PKT_SECURE) return;
        ciphertext_size = payload_size - MN_SECURE_OVERHEAD - 1u;
        inner_size = MN_HEADER_SIZE + ciphertext_size;
        if (inner_size > sizeof(plaintext)) return;
        memcpy(aad, packet, MN_HEADER_SIZE);
        aad[MN_HEADER_SIZE] = inner_type;
        if (!mn_crypto_aes256gcm_decrypt(instance->room_key, payload,
                                         aad, sizeof(aad),
                                         payload + MN_SECURE_NONCE_BYTES + 1u,
                                         ciphertext_size,
                                         payload + MN_SECURE_NONCE_BYTES + 1u + ciphertext_size,
                                         plaintext + MN_HEADER_SIZE))
            return;
        if (!mn_accept_secure_nonce(instance, from, payload)) return;
        /* Rebuild the authenticated inner header; only the payload is encrypted. */
        if (mn_make_packet(plaintext, inner_type, app_id, room_id, NULL,
                           ciphertext_size) != inner_size)
            return;
        mn_handle_packet_internal(instance, plaintext, inner_size, from, 1);
        return;
    }
    /* Once a room key is configured, reject every unauthenticated game packet. */
    if (instance->encryption_enabled && !authenticated) return;

    if (type == MN_PKT_DISCOVER && payload_size == 0 &&
        instance->info.role == MN_ROLE_HOST &&
        instance->info.status == MN_STATUS_HOSTING) {
        mn_send_offer(instance, from);
        return;
    }
    if (type == MN_PKT_OFFER && payload_size == MN_OFFER_SIZE &&
        instance->info.status == MN_STATUS_SEARCHING) {
        mn_add_room(instance, from, room_id, payload);
        return;
    }

    if (instance->info.role == MN_ROLE_HOST) {
        if (type == MN_PKT_JOIN && room_id == 0 &&
            payload_size == MN_DEVICE_ID_BYTES + MN_DEVICE_SECRET_BYTES +
                           MN_PLAYER_NAME_LENGTH) {
            mn_peer *peer = mn_find_peer_address(instance, from);
            const uint8_t *device_id = payload;
            const uint8_t *device_secret = payload + MN_DEVICE_ID_BYTES;
            uint8_t name[MN_PLAYER_NAME_LENGTH];
            size_t i, occupied = 0, paired_index;
            memcpy(name, payload + MN_DEVICE_ID_BYTES + MN_DEVICE_SECRET_BYTES,
                   sizeof(name));
            name[sizeof(name) - 1] = '\0';
            if (!mn_device_id_is_valid(device_id)) return;
            if (instance->encryption_enabled) {
                if (!mn_device_secret_is_valid(device_secret)) return;
            } else if (instance->pairing_required ||
                       mn_device_secret_is_valid(device_secret)) {
                return;
            }

            if (peer) {
                peer->silence = 0.0f;
                if (memcmp(peer->device_id, device_id, MN_DEVICE_ID_BYTES) != 0)
                    return;
                if (peer->pending_approval) {
                    if (!mn_secure_equal(peer->device_secret, device_secret,
                                         MN_DEVICE_SECRET_BYTES))
                        return;
                    if (peer->challenge_pending)
                        (void)mn_send_pairing_challenge(instance, peer);
                    return;
                }
                paired_index = mn_find_paired_device_index(instance, device_id);
                if (paired_index < MN_MAX_PAIRED_DEVICES &&
                    (!instance->encryption_enabled ||
                     !mn_secure_equal(instance->paired_device_secrets[paired_index],
                                      device_secret, MN_DEVICE_SECRET_BYTES)))
                    return;
                mn_send_welcome(instance, peer);
                mn_send_snapshot_to(instance, from);
                return;
            }

            /* The IPv4 allow-list is an independent network-level filter. */
            if (!mn_join_source_is_allowed(instance, from)) return;
            if (mn_active_player_count(instance) >= instance->info.max_players) return;
            for (i = 0; i < MN_MAX_PLAYERS; ++i) {
                if (instance->peers[i].active || instance->peers[i].pending_approval)
                    occupied++;
            }
            if (occupied >= (size_t)instance->info.max_players - 1u) return;
            for (i = 0; i < MN_MAX_PLAYERS; ++i) {
                if (!instance->peers[i].active &&
                    !instance->peers[i].pending_approval) break;
            }
            if (i == MN_MAX_PLAYERS) return;
            peer = &instance->peers[i];
            memset(peer, 0, sizeof(*peer));
            peer->address = *from;
            peer->pending_approval = 1;
            memcpy(peer->device_id, device_id, MN_DEVICE_ID_BYTES);
            memcpy(peer->device_secret, device_secret, MN_DEVICE_SECRET_BYTES);
            memcpy(peer->player.name, name, sizeof(name));
            peer->player.name[MN_PLAYER_NAME_LENGTH - 1] = '\0';
            peer->player.colour_rgba = 0xFFFFFFFFu;
            peer->player.rotation.w = 1.0f;

            paired_index = mn_find_paired_device_index(instance, device_id);
            if (paired_index < MN_MAX_PAIRED_DEVICES) {
                if (!instance->encryption_enabled ||
                    !mn_secure_equal(instance->paired_device_secrets[paired_index],
                                     device_secret, MN_DEVICE_SECRET_BYTES)) {
                    uint8_t denied[MN_MAX_DATAGRAM_SIZE];
                    size_t denied_size = mn_make_packet(denied, MN_PKT_PAIRING_DENIED,
                                                        instance->config.app_id, 0,
                                                        NULL, 0);
                    mn_send_to(instance, from, denied, denied_size);
                    mn_secure_zero(peer, sizeof(*peer));
                    return;
                }
                if (!mn_send_pairing_challenge(instance, peer)) {
                    mn_secure_zero(peer, sizeof(*peer));
                    mn_set_error(instance, "Could not create a device pairing challenge");
                }
                return;
            }

            if (instance->pairing_required) {
                mn_emit_pairing_event(instance, MN_EVENT_PAIRING_REQUEST,
                                      peer, "A device is requesting to join this room");
                return;
            }
            mn_activate_peer(instance, peer);
            return;
        }
        if (room_id != instance->info.room_id) return;
        if (type == MN_PKT_PAIRING_PROOF &&
            payload_size == MN_DEVICE_ID_BYTES + 32u) {
            mn_peer *peer = mn_find_peer_address(instance, from);
            uint8_t expected_proof[32];
            if (!peer || !peer->pending_approval || !peer->challenge_pending ||
                !mn_secure_equal(peer->device_id, payload, MN_DEVICE_ID_BYTES))
                return;
            if (!mn_make_pairing_proof(peer->device_secret, peer->device_id,
                                       peer->challenge_nonce, instance->info.room_id,
                                       instance->config.app_id, expected_proof) ||
                !mn_secure_equal(expected_proof, payload + MN_DEVICE_ID_BYTES,
                                 sizeof(expected_proof))) {
                uint8_t denied[MN_MAX_DATAGRAM_SIZE];
                size_t denied_size = mn_make_packet(denied, MN_PKT_PAIRING_DENIED,
                                                    instance->config.app_id, 0,
                                                    NULL, 0);
                mn_send_to(instance, &peer->address, denied, denied_size);
                mn_emit_pairing_event(instance, MN_EVENT_PAIRING_REJECTED,
                                      peer, "Device pairing proof was invalid");
                mn_secure_zero(peer, sizeof(*peer));
                return;
            }
            peer->challenge_pending = 0;
            mn_activate_peer(instance, peer);
            mn_emit_pairing_event(instance, MN_EVENT_PAIRING_APPROVED,
                                  peer, "Device proved its paired identity");
            return;
        }
        if (type == MN_PKT_STATE && payload_size == 12u + MN_WIRE_PLAYER_SIZE) {
            uint64_t token = mn_read_u64(payload);
            uint32_t sequence = mn_read_u32(payload + 8);
            mn_peer *peer = mn_find_peer_token(instance, token);
            mn_player player;
            if (!peer || !mn_same_address(&peer->address, from)) return;
            if (peer->has_state_sequence &&
                !mn_sequence_is_newer(sequence, peer->last_state_sequence)) return;
            if (peer->state_cooldown > 0.0f) return;

            /* Consume the sequence and cooldown even for rejected state so a
               peer cannot bypass the packet rate limit with invalid payloads. */
            peer->last_state_sequence = sequence;
            peer->has_state_sequence = 1;
            peer->state_cooldown = instance->config.heartbeat_seconds * 0.5f;
            if (peer->state_cooldown < 0.01f) peer->state_cooldown = 0.01f;

            if (!mn_decode_player(&player, payload + 12)) return;
            player.id = peer->id;
            mn_copy_text(player.name, sizeof(player.name), peer->player.name);
            if (!mn_player_within_state_limits(instance, &player)) return;
            if (instance->state_validator &&
                !instance->state_validator(&peer->player, &player,
                                           instance->state_validator_user_data))
                return;

            /* Only accepted state refreshes the peer timeout or replaces the
               last known-good state exposed to the rest of the application. */
            peer->silence = 0.0f;
            peer->player = player;
            mn_refresh_host_players(instance);
            return;
        }
        if (type == MN_PKT_LEAVE && payload_size == 8u) {
            mn_peer *peer = mn_find_peer_token(instance, mn_read_u64(payload));
            if (peer && mn_same_address(&peer->address, from)) {
                mn_remove_peer(instance, peer, "Player left the room");
                mn_broadcast_snapshot(instance);
            }
            return;
        }
        if (type == MN_PKT_MESSAGE) {
            mn_handle_host_message(instance, payload, payload_size, from);
            return;
        }
        if (type == MN_PKT_RELIABLE) {
            mn_handle_host_reliable(instance, payload, payload_size, from);
            return;
        }
        if (type == MN_PKT_RELIABLE_ACK) {
            mn_handle_host_reliable_ack(instance, payload, payload_size, from);
            return;
        }
        if (type == MN_PKT_PING) {
            mn_handle_host_ping(instance, payload, payload_size, from);
            return;
        }
        if (type == MN_PKT_PONG) {
            mn_handle_host_pong(instance, payload, payload_size, from);
            return;
        }
        return;
    }

    if (instance->info.role == MN_ROLE_CLIENT &&
        mn_same_address(from, &instance->server)) {
        instance->server_silence = 0.0f;
        if (type == MN_PKT_PAIRING_DENIED && room_id == 0 &&
            payload_size == 0 && instance->info.status == MN_STATUS_CONNECTING) {
            instance->info.role = MN_ROLE_NONE;
            instance->info.status = MN_STATUS_OFFLINE;
            instance->connect_elapsed = 0.0f;
            mn_emit_simple_event(instance, MN_EVENT_PAIRING_REJECTED,
                                 "The host rejected this device's pairing request", 0);
            return;
        }
        if (type == MN_PKT_PAIRING_CHALLENGE &&
            instance->info.status == MN_STATUS_CONNECTING &&
            payload_size == MN_DEVICE_ID_BYTES + 32u &&
            room_id != 0 &&
            mn_secure_equal(payload, instance->device_id, MN_DEVICE_ID_BYTES)) {
            uint8_t proof[32], proof_payload[MN_DEVICE_ID_BYTES + 32u];
            uint8_t proof_packet[MN_MAX_DATAGRAM_SIZE];
            size_t proof_packet_size;
            if (!instance->encryption_enabled ||
                !mn_make_pairing_proof(instance->device_secret, instance->device_id,
                                       payload + MN_DEVICE_ID_BYTES, room_id,
                                       instance->config.app_id, proof))
                return;
            memcpy(proof_payload, instance->device_id, MN_DEVICE_ID_BYTES);
            memcpy(proof_payload + MN_DEVICE_ID_BYTES, proof, sizeof(proof));
            proof_packet_size = mn_make_packet(proof_packet, MN_PKT_PAIRING_PROOF,
                                               instance->config.app_id, room_id,
                                               proof_payload, sizeof(proof_payload));
            if (proof_packet_size != 0)
                mn_send_to(instance, &instance->server, proof_packet, proof_packet_size);
            return;
        }
        if (room_id != instance->info.room_id &&
            !(type == MN_PKT_WELCOME && instance->info.status == MN_STATUS_CONNECTING)) return;
        if (type == MN_PKT_WELCOME && payload_size == MN_WELCOME_SIZE &&
            instance->info.status == MN_STATUS_CONNECTING) {
            instance->info.local_player_id = mn_read_u32(payload);
            instance->session_token = mn_read_u64(payload + 4);
            instance->info.max_players = payload[12];
            instance->info.room_id = room_id;
            memcpy(instance->info.room_name, payload + 13, MN_ROOM_NAME_LENGTH);
            instance->info.room_name[MN_ROOM_NAME_LENGTH - 1] = '\0';
            instance->local_player.id = instance->info.local_player_id;
            mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name),
                         instance->info.local_name);
            instance->players[0] = instance->local_player;
            instance->info.status = MN_STATUS_CONNECTED;
            instance->info.player_count = 1;
            mn_emit_simple_event(instance, MN_EVENT_CONNECTED, "Connected to room",
                                 instance->info.local_player_id);
            return;
        }
        if (type == MN_PKT_START && payload_size == 0) {
            if (!instance->info.match_started) {
                instance->info.match_started = 1;
                mn_emit_simple_event(instance, MN_EVENT_MATCH_STARTED,
                                     "Host started the match", 0);
            }
            return;
        }
        if (type == MN_PKT_SNAPSHOT && payload_size >= 6u) {
            mn_player incoming[MN_MAX_PLAYERS];
            uint32_t sequence = mn_read_u32(payload);
            size_t count = payload[4], i, expected;
            int was_started = instance->info.match_started;
            if (instance->has_snapshot_sequence &&
                !mn_sequence_is_newer(sequence, instance->last_snapshot_sequence)) return;
            if (count == 0 || count > MN_MAX_PLAYERS) return;
            expected = 6u + count * MN_WIRE_PLAYER_SIZE;
            if (payload_size != expected || payload[5] > 1u) return;
            memset(incoming, 0, sizeof(incoming));
            for (i = 0; i < count; ++i) {
                if (!mn_decode_player(&incoming[i], payload + 6u + i * MN_WIRE_PLAYER_SIZE))
                    return;
            }



            for (i = 0; i < count; ++i) {
                size_t old_index;
                int found = 0;
                if (incoming[i].id == instance->info.local_player_id) continue;
                for (old_index = 0; old_index < instance->info.player_count; ++old_index) {
                    if (instance->players[old_index].id == incoming[i].id) { found = 1; break; }
                }
                if (!found) {
                    mn_event event;
                    memset(&event, 0, sizeof(event));
                    event.type = MN_EVENT_PLAYER_JOINED;
                    event.player_id = incoming[i].id;
                    event.player = incoming[i];
                    mn_push_event(instance, &event);
                }
            }
            for (i = 0; i < instance->info.player_count; ++i) {
                size_t new_index;
                uint32_t old_id = instance->players[i].id;
                int found = 0;
                if (old_id == 0 || old_id == instance->info.local_player_id) continue;
                for (new_index = 0; new_index < count; ++new_index) {
                    if (incoming[new_index].id == old_id) { found = 1; break; }
                }
                if (!found) mn_emit_simple_event(instance, MN_EVENT_PLAYER_LEFT,
                                                  "Player left the room", old_id);
            }
            memset(instance->players, 0, sizeof(instance->players));
            memcpy(instance->players, incoming, count * sizeof(mn_player));
            instance->info.player_count = (uint8_t)count;
            instance->info.match_started = payload[5];
            instance->last_snapshot_sequence = sequence;
            instance->has_snapshot_sequence = 1;
            if (!was_started && instance->info.match_started)
                mn_emit_simple_event(instance, MN_EVENT_MATCH_STARTED,
                                     "Host started the match", 0);
            return;
        }
        if (type == MN_PKT_MESSAGE) {
            mn_handle_client_message(instance, payload, payload_size);
            return;
        }
        if (type == MN_PKT_RELIABLE) {
            mn_handle_client_reliable(instance, payload, payload_size);
            return;
        }
        if (type == MN_PKT_RELIABLE_ACK) {
            mn_handle_client_reliable_ack(instance, payload, payload_size);
            return;
        }
        if (type == MN_PKT_PING) {
            mn_handle_client_ping(instance, payload, payload_size);
            return;
        }
        if (type == MN_PKT_PONG) {
            mn_handle_client_pong(instance, payload, payload_size);
            return;
        }
    }
}

static void mn_handle_packet(mn_instance *instance, const uint8_t *packet,
                             size_t packet_size, const struct sockaddr_in *from) {
    mn_handle_packet_internal(instance, packet, packet_size, from, 0);
}

void mn_player_state_limits_init(mn_player_state_limits *limits) {
    if (!limits) return;
    limits->max_abs_position = 1000000.0f;
    limits->max_abs_velocity = 1000000.0f;
}

void mn_config_init(mn_config *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->port = MN_DEFAULT_PORT;
    config->app_id = MN_DEFAULT_APP_ID;
    config->max_players = MN_MAX_PLAYERS;
    config->enable_broadcast = 1;
    config->discovery_seconds = 3.0f;
    config->heartbeat_seconds = 0.10f;
    config->timeout_seconds = 8.0f;
}

mn_instance *mn_create(const mn_config *config) {
    mn_instance *instance;
    mn_config defaults;
    mn_config_init(&defaults);
    instance = (mn_instance *)calloc(1, sizeof(*instance));
    if (!instance) return NULL;
    instance->config = config ? *config : defaults;
    mn_player_state_limits_init(&instance->state_limits);
    if (instance->config.port == 0) instance->config.port = defaults.port;
    if (instance->config.app_id == 0) instance->config.app_id = defaults.app_id;
    if (instance->config.max_players < 2 || instance->config.max_players > MN_MAX_PLAYERS)
        instance->config.max_players = MN_MAX_PLAYERS;
    if (instance->config.discovery_seconds <= 0.0f) instance->config.discovery_seconds = defaults.discovery_seconds;
    if (instance->config.heartbeat_seconds <= 0.0f) instance->config.heartbeat_seconds = defaults.heartbeat_seconds;
    if (instance->config.timeout_seconds <= 0.0f) instance->config.timeout_seconds = defaults.timeout_seconds;
    instance->socket = MN_BAD_SOCKET;
    instance->mdns_socket = MN_BAD_SOCKET;
#ifdef _WIN32
    {
        WSADATA winsock_data;
        if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
            free(instance);
            return NULL;
        }
    }
#endif
    if (!mn_random_bytes(instance->device_id, sizeof(instance->device_id)) ||
        !mn_random_bytes(instance->device_secret, sizeof(instance->device_secret)) ||
        !mn_random_bytes((uint8_t *)&instance->secure_nonce_prefix,
                         sizeof(instance->secure_nonce_prefix))) {
#ifdef _WIN32
        WSACleanup();
#endif
        free(instance);
        return NULL;
    }
    instance->socket = mn_open_socket(instance, 0);
    if (instance->socket == MN_BAD_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        free(instance);
        return NULL;
    }


    instance->mdns_socket = mn_open_mdns_socket(instance);
    instance->info.mdns_available = instance->mdns_socket != MN_BAD_SOCKET ? 1u : 0u;
    instance->ready = 1;
    instance->info.status = MN_STATUS_OFFLINE;
    instance->info.role = MN_ROLE_NONE;
    instance->info.app_id = instance->config.app_id;
    instance->info.max_players = instance->config.max_players;
    instance->info.local_port = instance->local_port;
    instance->local_player.colour_rgba = 0xFFFFFFFFu;
    instance->local_player.rotation.w = 1.0f;
    return instance;
}

void mn_destroy(mn_instance *instance) {
    if (!instance) return;
    mn_leave(instance);
    mn_close_current_socket(instance);
    if (instance->mdns_socket != MN_BAD_SOCKET)
        mn_close_socket(instance->mdns_socket);
#ifdef _WIN32
    WSACleanup();
#endif
    mn_secure_zero(instance->room_key, sizeof(instance->room_key));
    mn_secure_zero(instance->device_secret, sizeof(instance->device_secret));
    mn_secure_zero(instance->paired_device_secrets, sizeof(instance->paired_device_secrets));
    memset(instance, 0, sizeof(*instance));
    free(instance);
}

static int mn_host_internal(mn_instance *instance, const char *player_name,
                            const char *room_name, uint8_t max_players,
                            const char *local_ipv4) {
    mn_socket new_socket;
    uint32_t room_id;
    if (!instance || !instance->ready) return 0;
    if (instance->info.role == MN_ROLE_HOST) mn_leave(instance);
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    instance->discovery_mdns = 0;
    instance->discovery_auto = 0;
    max_players = max_players < 2 ? 2 : (max_players > MN_MAX_PLAYERS ? MN_MAX_PLAYERS : max_players);
    mn_close_current_socket(instance);
    new_socket = mn_open_socket_on(instance, instance->config.port, local_ipv4);
    if (new_socket == MN_BAD_SOCKET) {
        instance->socket = mn_open_socket(instance, 0);
        instance->info.local_port = instance->local_port;
        return 0;
    }
    instance->socket = new_socket;
    instance->info.local_port = instance->local_port;
    memset(instance->peers, 0, sizeof(instance->peers));
    memset(instance->players, 0, sizeof(instance->players));
    instance->room_count = 0;
    instance->session_token = 0;
    instance->info.ping_pending = 0;
    instance->info.ping_available = 0;
    instance->info.ping_ms = 0.0f;
    instance->ping_elapsed = 0.0f;
    instance->ping_pending_id = 0;
    instance->ping_pending_target = 0;
    {
        size_t reliable_index;
        for (reliable_index = 0; reliable_index < MN_MAX_RELIABLE_PENDING;
             ++reliable_index) {
            mn_reliable_entry *entry = &instance->reliable[reliable_index];
            if (entry->active) {
                mn_emit_reliable_status(instance, MN_EVENT_MESSAGE_FAILED, entry,
                                         entry->peer_id,
                                         "Session ended before reliable delivery was acknowledged");
            }
        }
    }
    memset(instance->reliable, 0, sizeof(instance->reliable));
    memset(instance->reliable_seen, 0, sizeof(instance->reliable_seen));
    instance->next_reliable_id = 0;
    instance->reliable_seen_age = 0;
    instance->next_state_sequence = 0;
    instance->snapshot_sequence = 0;
    instance->last_snapshot_sequence = 0;
    instance->has_snapshot_sequence = 0;
    instance->next_player_id = 2;
    instance->info.role = MN_ROLE_HOST;
    instance->info.status = MN_STATUS_HOSTING;
    instance->info.local_player_id = 1;
    instance->info.room_id = 0;
    if (!mn_random_u32(&room_id)) {
        mn_set_error(instance, "Secure random number generation failed");
        mn_leave(instance);
        return 0;
    }
    instance->info.room_id = room_id;
    instance->info.max_players = max_players;
    instance->info.match_started = 0;
    mn_copy_text(instance->info.local_name, sizeof(instance->info.local_name), player_name);
    mn_copy_text(instance->info.room_name, sizeof(instance->info.room_name), room_name);
    memset(&instance->local_player, 0, sizeof(instance->local_player));
    instance->local_player.id = 1;
    instance->local_player.colour_rgba = 0xFFFFFFFFu;
    instance->local_player.rotation.w = 1.0f;
    mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name), player_name);
    instance->info.last_error[0] = '\0';
    instance->snapshot_tick = 0.0f;
    mn_refresh_host_players(instance);
    return 1;
}


int mn_host(mn_instance *instance, const char *player_name,
            const char *room_name, uint8_t max_players) {
    return mn_host_internal(instance, player_name, room_name, max_players, NULL);
}

int mn_host_on(mn_instance *instance, const char *player_name,
               const char *room_name, uint8_t max_players,
               const char *local_ipv4) {
    if (!local_ipv4 || !local_ipv4[0]) {
        if (instance) mn_set_error(instance, "The selected host interface address is empty");
        return 0;
    }
    return mn_host_internal(instance, player_name, room_name, max_players, local_ipv4);
}

int mn_discover(mn_instance *instance) {
    if (!instance || !instance->ready) return 0;
    if (!instance->config.enable_broadcast) {
        mn_set_error(instance, "Broadcast discovery is disabled; use mn_discover_at or mn_join");
        return 0;
    }
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before searching");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    instance->room_count = 0;
    memset(instance->rooms, 0, sizeof(instance->rooms));
    instance->discovery_left = instance->config.discovery_seconds;
    instance->discovery_tick = 0.0f;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_SEARCHING;
    instance->info.last_error[0] = '\0';
    instance->discovery_mdns = 0;
    instance->discovery_auto = 0;
    instance->discovery_target_valid = 0;
    mn_send_discover(instance);
    return 1;
}



int mn_discover_auto(mn_instance *instance) {
    if (!instance || !instance->ready) return 0;
    if (!instance->config.enable_broadcast && instance->mdns_socket == MN_BAD_SOCKET) {
        mn_set_error(instance, "No automatic discovery transport is available; use mn_discover_at or mn_join");
        return 0;
    }
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before searching");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    instance->room_count = 0;
    memset(instance->rooms, 0, sizeof(instance->rooms));
    instance->discovery_left = instance->config.discovery_seconds;
    instance->discovery_tick = 0.0f;
    instance->discovery_target_valid = 0;
    instance->discovery_mdns = 0;
    instance->discovery_auto = 1;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_SEARCHING;
    instance->info.last_error[0] = '\0';
    mn_send_auto_discover(instance);
    return 1;
}

int mn_discover_mdns(mn_instance *instance) {
    if (!instance || !instance->ready) return 0;
    if (instance->mdns_socket == MN_BAD_SOCKET) {
        mn_set_error(instance, "mDNS is unavailable on this system or UDP port 5353 could not be bound");
        return 0;
    }
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before searching");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    instance->room_count = 0;
    memset(instance->rooms, 0, sizeof(instance->rooms));
    instance->discovery_left = instance->config.discovery_seconds;
    instance->discovery_tick = 0.0f;
    instance->discovery_target_valid = 0;
    instance->discovery_auto = 0;
    instance->discovery_mdns = 1;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_SEARCHING;
    instance->info.last_error[0] = '\0';
    mn_send_mdns_query(instance);
    return 1;
}

int mn_discover_at(mn_instance *instance, const char *ipv4_or_hostname,
                   uint16_t port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct sockaddr_in destination;
    int resolve_result;
    if (!instance || !instance->ready) return 0;
    if (!ipv4_or_hostname || !ipv4_or_hostname[0]) {
        mn_set_error(instance, "Discovery target address is empty");
        return 0;
    }
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before searching");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    resolve_result = getaddrinfo(ipv4_or_hostname, NULL, &hints, &result);
    if (resolve_result != 0 || !result) {
        if (result) freeaddrinfo(result);
        mn_set_error(instance, "Could not resolve the discovery target as IPv4");
        return 0;
    }
    memset(&destination, 0, sizeof(destination));
    memcpy(&destination, result->ai_addr, sizeof(destination));
    destination.sin_port = htons(port ? port : instance->config.port);
    freeaddrinfo(result);

    instance->room_count = 0;
    memset(instance->rooms, 0, sizeof(instance->rooms));
    instance->discovery_left = instance->config.discovery_seconds;
    instance->discovery_tick = 0.0f;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_SEARCHING;
    instance->info.last_error[0] = '\0';
    instance->discovery_mdns = 0;
    instance->discovery_auto = 0;
    instance->discovery_target = destination;
    instance->discovery_target_valid = 1;
    mn_send_discover_to(instance, &instance->discovery_target);
    return 1;
}




int mn_discover_on(mn_instance *instance, const mn_interface *network_interface) {
    struct sockaddr_in destination;
    if (!instance || !instance->ready || !network_interface) return 0;
    if (!network_interface->address[0] || !network_interface->broadcast[0]) {
        mn_set_error(instance, "The selected interface has no IPv4 broadcast address");
        return 0;
    }
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before searching");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    if (!mn_rebind_socket(instance, network_interface->address)) {
        mn_set_error(instance, "Could not bind discovery to the selected interface");
        return 0;
    }
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(instance->config.port);
    if (inet_pton(AF_INET, network_interface->broadcast, &destination.sin_addr) != 1) {
        mn_set_error(instance, "The selected interface broadcast address is invalid");
        return 0;
    }
    instance->room_count = 0;
    memset(instance->rooms, 0, sizeof(instance->rooms));
    instance->discovery_left = instance->config.discovery_seconds;
    instance->discovery_tick = 0.0f;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_SEARCHING;
    instance->info.last_error[0] = '\0';
    instance->discovery_mdns = 0;
    instance->discovery_auto = 0;
    instance->discovery_target = destination;
    instance->discovery_target_valid = 1;
    mn_send_discover_to(instance, &instance->discovery_target);
    return 1;
}

void mn_stop_discovery(mn_instance *instance) {
    if (!instance || instance->info.status != MN_STATUS_SEARCHING) return;
    instance->info.status = MN_STATUS_OFFLINE;
    instance->info.role = MN_ROLE_NONE;
    instance->discovery_left = 0.0f;
    instance->discovery_target_valid = 0;
    instance->discovery_mdns = 0;
    instance->discovery_auto = 0;
}

size_t mn_get_rooms(const mn_instance *instance, mn_room *rooms, size_t capacity) {
    size_t count;
    if (!instance) return 0;
    count = instance->room_count < capacity ? instance->room_count : capacity;
    if (rooms && count) memcpy(rooms, instance->rooms, count * sizeof(mn_room));
    return instance->room_count;
}

static int mn_join_internal(mn_instance *instance, const char *host, uint16_t port,
                            const char *player_name, const char *local_ipv4) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct sockaddr_in server;
    int resolve_result;
    if (!instance || !instance->ready) return 0;
    if (!host || !host[0]) {
        mn_set_error(instance, "Host address is empty");
        return 0;
    }
    if (instance->info.role == MN_ROLE_HOST) {
        mn_set_error(instance, "Leave the hosted room before joining another room");
        return 0;
    }
    if (instance->info.role == MN_ROLE_CLIENT) mn_leave(instance);
    instance->discovery_mdns = 0;
    instance->discovery_auto = 0;
    if (!mn_rebind_socket(instance, local_ipv4)) {
        mn_set_error(instance, "Could not bind the client socket to the selected local interface");
        return 0;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    resolve_result = getaddrinfo(host, NULL, &hints, &result);
    if (resolve_result != 0 || !result) {
        mn_set_error(instance, "Could not resolve the host as an IPv4 address");
        if (result) freeaddrinfo(result);
        return 0;
    }
    memset(&server, 0, sizeof(server));
    memcpy(&server, result->ai_addr, sizeof(server));
    server.sin_port = htons(port ? port : instance->config.port);
    freeaddrinfo(result);
    instance->server = server;
    instance->discovery_target_valid = 0;
    instance->next_state_sequence = 0;
    instance->last_snapshot_sequence = 0;
    instance->has_snapshot_sequence = 0;
    mn_copy_text(instance->info.local_name, sizeof(instance->info.local_name), player_name);
    mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name), player_name);
    instance->local_player.id = 0;
    if (instance->local_player.colour_rgba == 0) instance->local_player.colour_rgba = 0xFFFFFFFFu;
    if (instance->local_player.rotation.w == 0.0f) instance->local_player.rotation.w = 1.0f;
    instance->info.role = MN_ROLE_CLIENT;
    instance->info.status = MN_STATUS_CONNECTING;
    instance->info.local_player_id = 0;
    instance->info.room_id = 0;
    instance->info.player_count = 0;
    instance->info.match_started = 0;
    instance->info.max_players = instance->config.max_players;
    instance->info.room_name[0] = '\0';
    instance->session_token = 0;
    instance->info.ping_pending = 0;
    instance->info.ping_available = 0;
    instance->info.ping_ms = 0.0f;
    instance->ping_elapsed = 0.0f;
    instance->ping_pending_id = 0;
    instance->ping_pending_target = 0;
    memset(instance->reliable, 0, sizeof(instance->reliable));
    memset(instance->reliable_seen, 0, sizeof(instance->reliable_seen));
    instance->next_reliable_id = 0;
    instance->reliable_seen_age = 0;
    instance->connect_elapsed = 0.0f;
    instance->server_silence = 0.0f;
    instance->heartbeat_tick = 0.0f;
    instance->info.last_error[0] = '\0';
    instance->discovery_target_valid = 0;
    memset(instance->players, 0, sizeof(instance->players));
    mn_send_join(instance);
    return 1;
}


int mn_join(mn_instance *instance, const char *host, uint16_t port,
            const char *player_name) {
    return mn_join_internal(instance, host, port, player_name, NULL);
}

int mn_join_from(mn_instance *instance, const char *local_ipv4,
                 const char *host, uint16_t port, const char *player_name) {
    if (!local_ipv4 || !local_ipv4[0]) {
        if (instance) mn_set_error(instance, "The selected client interface address is empty");
        return 0;
    }
    return mn_join_internal(instance, host, port, player_name, local_ipv4);
}

int mn_join_room(mn_instance *instance, const mn_room *room,
                 const char *player_name) {
    if (!room) return 0;
    return mn_join(instance, room->address, room->port, player_name);
}

int mn_start_match(mn_instance *instance) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    size_t size, i;
    if (!instance || !instance->ready || instance->info.role != MN_ROLE_HOST ||
        instance->info.status != MN_STATUS_HOSTING) {
        if (instance) mn_set_error(instance, "Only the room host can start the match");
        return 0;
    }
    if (instance->info.match_started) return 1;
    instance->info.match_started = 1;
    size = mn_make_packet(packet, MN_PKT_START, instance->config.app_id,
                          instance->info.room_id, NULL, 0);
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        if (instance->peers[i].active)
            mn_send_to(instance, &instance->peers[i].address, packet, size);
    }


    mn_emit_simple_event(instance, MN_EVENT_MATCH_STARTED, "Host started the match", 0);
    return 1;
}

void mn_leave(mn_instance *instance) {
    mn_role previous_role;
    if (!instance || !instance->ready) return;
    previous_role = instance->info.role;
    if (previous_role == MN_ROLE_CLIENT && instance->info.status == MN_STATUS_CONNECTED)
        mn_send_leave(instance);
    if (previous_role == MN_ROLE_HOST) {
        mn_close_current_socket(instance);
        instance->socket = mn_open_socket(instance, 0);
        instance->info.local_port = instance->local_port;
    }
    if (previous_role == MN_ROLE_CLIENT && instance->info.status == MN_STATUS_CONNECTED)
        mn_emit_simple_event(instance, MN_EVENT_DISCONNECTED, "Left the room", instance->info.local_player_id);
    memset(instance->peers, 0, sizeof(instance->peers));
    memset(instance->players, 0, sizeof(instance->players));
    memset(&instance->local_player, 0, sizeof(instance->local_player));
    instance->local_player.colour_rgba = 0xFFFFFFFFu;
    instance->local_player.rotation.w = 1.0f;
    instance->info.role = MN_ROLE_NONE;
    instance->info.status = MN_STATUS_OFFLINE;
    instance->info.local_player_id = 0;
    instance->info.room_id = 0;
    instance->info.player_count = 0;
    instance->info.max_players = instance->config.max_players;
    instance->info.match_started = 0;
    instance->info.room_name[0] = '\0';
    instance->info.local_name[0] = '\0';
    instance->info.last_error[0] = '\0';
    instance->session_token = 0;
    instance->info.ping_pending = 0;
    instance->info.ping_available = 0;
    instance->info.ping_ms = 0.0f;
    instance->ping_elapsed = 0.0f;
    instance->ping_pending_id = 0;
    instance->ping_pending_target = 0;
    memset(instance->reliable, 0, sizeof(instance->reliable));
    memset(instance->reliable_seen, 0, sizeof(instance->reliable_seen));
    instance->next_reliable_id = 0;
    instance->reliable_seen_age = 0;
    instance->next_state_sequence = 0;
    instance->snapshot_sequence = 0;
    instance->last_snapshot_sequence = 0;
    instance->has_snapshot_sequence = 0;
    instance->discovery_target_valid = 0;
    instance->discovery_mdns = 0;
    memset(&instance->discovery_target, 0, sizeof(instance->discovery_target));
    instance->room_count = 0;
    instance->discovery_left = 0.0f;
    instance->discovery_target_valid = 0;
    memset(&instance->discovery_target, 0, sizeof(instance->discovery_target));
    instance->connect_elapsed = 0.0f;
    instance->server_silence = 0.0f;
}

int mn_set_local_player(mn_instance *instance, const mn_player *player) {
    if (!instance || !player || !mn_player_is_valid(player)) return 0;
    instance->local_player = *player;
    instance->local_player.id = instance->info.local_player_id;
    mn_copy_text(instance->local_player.name, sizeof(instance->local_player.name),
                 instance->info.local_name);
    if (instance->info.role == MN_ROLE_HOST) mn_refresh_host_players(instance);
    return 1;
}

int mn_set_player_state_validation(mn_instance *instance,
                                   const mn_player_state_limits *limits,
                                   mn_player_state_validator validator,
                                   void *user_data) {
    mn_player_state_limits defaults;
    if (!instance || !instance->ready) return 0;
    if (limits) {
        if (!mn_float_is_valid(limits->max_abs_position) ||
            !mn_float_is_valid(limits->max_abs_velocity) ||
            limits->max_abs_position < 0.0f ||
            limits->max_abs_velocity < 0.0f)
            return 0;
        instance->state_limits = *limits;
    } else {
        mn_player_state_limits_init(&defaults);
        instance->state_limits = defaults;
    }
    instance->state_validator = validator;
    instance->state_validator_user_data = user_data;
    return 1;
}

int mn_set_join_allowlist_enabled(mn_instance *instance, int enabled) {
    if (!instance || !instance->ready || (enabled != 0 && enabled != 1))
        return 0;
    instance->join_allowlist_enabled = enabled;
    return 1;
}

int mn_allow_join_ipv4(mn_instance *instance, const char *ipv4_address) {
    char normalized[48];
    size_t i;
    if (!instance || !instance->ready ||
        !mn_normalize_ipv4(ipv4_address, normalized))
        return 0;
    for (i = 0; i < instance->join_allowlist_count; ++i) {
        if (strcmp(instance->join_allowlist[i], normalized) == 0)
            return 1;
    }
    if (instance->join_allowlist_count >= MN_MAX_JOIN_ALLOWLIST)
        return 0;
    mn_copy_text(instance->join_allowlist[instance->join_allowlist_count],
                 sizeof(instance->join_allowlist[0]), normalized);
    instance->join_allowlist_count++;
    return 1;
}

int mn_disallow_join_ipv4(mn_instance *instance, const char *ipv4_address) {
    char normalized[48];
    size_t i;
    if (!instance || !instance->ready ||
        !mn_normalize_ipv4(ipv4_address, normalized))
        return 0;
    for (i = 0; i < instance->join_allowlist_count; ++i) {
        if (strcmp(instance->join_allowlist[i], normalized) == 0) {
            size_t remaining = instance->join_allowlist_count - i - 1u;
            if (remaining != 0u) {
                memmove(instance->join_allowlist[i],
                        instance->join_allowlist[i + 1u],
                        remaining * sizeof(instance->join_allowlist[0]));
            }
            instance->join_allowlist_count--;
            memset(instance->join_allowlist[instance->join_allowlist_count],
                   0, sizeof(instance->join_allowlist[0]));
            return 1;
        }
    }
    return 0;
}

void mn_clear_join_allowlist(mn_instance *instance) {
    if (!instance || !instance->ready) return;
    memset(instance->join_allowlist, 0, sizeof(instance->join_allowlist));
    instance->join_allowlist_count = 0;
}

int mn_get_device_id(const mn_instance *instance,
                     uint8_t out_device_id[MN_DEVICE_ID_BYTES]) {
    if (!instance || !out_device_id) return 0;
    memcpy(out_device_id, instance->device_id, MN_DEVICE_ID_BYTES);
    return 1;
}

int mn_get_device_secret(const mn_instance *instance,
                         uint8_t out_secret[MN_DEVICE_SECRET_BYTES]) {
    if (!instance || !out_secret) return 0;
    memcpy(out_secret, instance->device_secret, MN_DEVICE_SECRET_BYTES);
    return 1;
}

int mn_set_device_identity(mn_instance *instance,
                           const uint8_t device_id[MN_DEVICE_ID_BYTES],
                           const uint8_t device_secret[MN_DEVICE_SECRET_BYTES]) {
    if (!instance || !instance->ready ||
        instance->info.role != MN_ROLE_NONE ||
        instance->info.status != MN_STATUS_OFFLINE ||
        !mn_device_id_is_valid(device_id) ||
        !mn_device_secret_is_valid(device_secret))
        return 0;
    memcpy(instance->device_id, device_id, MN_DEVICE_ID_BYTES);
    mn_secure_zero(instance->device_secret, sizeof(instance->device_secret));
    memcpy(instance->device_secret, device_secret, MN_DEVICE_SECRET_BYTES);
    return 1;
}

int mn_set_pairing_required(mn_instance *instance, int required) {
    if (!instance || !instance->ready || (required != 0 && required != 1) ||
        instance->info.role == MN_ROLE_CLIENT ||
        (required && !instance->encryption_enabled))
        return 0;
    instance->pairing_required = required;
    return 1;
}

int mn_pair_device(mn_instance *instance,
                   const uint8_t device_id[MN_DEVICE_ID_BYTES],
                   const uint8_t device_secret[MN_DEVICE_SECRET_BYTES]) {
    size_t index;
    if (!instance || !instance->ready || !instance->encryption_enabled ||
        !mn_device_id_is_valid(device_id) ||
        !mn_device_secret_is_valid(device_secret))
        return 0;
    index = mn_find_paired_device_index(instance, device_id);
    if (index < MN_MAX_PAIRED_DEVICES) {
        if (!mn_secure_equal(instance->paired_device_secrets[index],
                             device_secret, MN_DEVICE_SECRET_BYTES))
            return 0;
        return 1;
    }
    if (instance->paired_device_count >= MN_MAX_PAIRED_DEVICES) return 0;
    index = instance->paired_device_count++;
    memcpy(instance->paired_devices[index], device_id, MN_DEVICE_ID_BYTES);
    memcpy(instance->paired_device_secrets[index], device_secret,
           MN_DEVICE_SECRET_BYTES);
    return 1;
}

int mn_get_paired_device_secret(const mn_instance *instance,
                                const uint8_t device_id[MN_DEVICE_ID_BYTES],
                                uint8_t out_secret[MN_DEVICE_SECRET_BYTES]) {
    size_t index;
    if (!instance || !device_id || !out_secret) return 0;
    index = mn_find_paired_device_index(instance, device_id);
    if (index >= MN_MAX_PAIRED_DEVICES) return 0;
    memcpy(out_secret, instance->paired_device_secrets[index],
           MN_DEVICE_SECRET_BYTES);
    return 1;
}

int mn_unpair_device(mn_instance *instance,
                     const uint8_t device_id[MN_DEVICE_ID_BYTES]) {
    size_t i;
    if (!instance || !instance->ready || !mn_device_id_is_valid(device_id)) return 0;
    for (i = 0; i < instance->paired_device_count; ++i) {
        if (memcmp(instance->paired_devices[i], device_id, MN_DEVICE_ID_BYTES) == 0) {
            size_t remaining = instance->paired_device_count - i - 1u;
            if (remaining != 0u) {
                memmove(instance->paired_devices[i], instance->paired_devices[i + 1u],
                        remaining * MN_DEVICE_ID_BYTES);
                memmove(instance->paired_device_secrets[i],
                        instance->paired_device_secrets[i + 1u],
                        remaining * MN_DEVICE_SECRET_BYTES);
            }
            instance->paired_device_count--;
            memset(instance->paired_devices[instance->paired_device_count], 0,
                   MN_DEVICE_ID_BYTES);
            mn_secure_zero(instance->paired_device_secrets[instance->paired_device_count],
                           MN_DEVICE_SECRET_BYTES);
            return 1;
        }
    }
    return 0;
}

void mn_clear_paired_devices(mn_instance *instance) {
    if (!instance || !instance->ready) return;
    memset(instance->paired_devices, 0, sizeof(instance->paired_devices));
    mn_secure_zero(instance->paired_device_secrets,
                   sizeof(instance->paired_device_secrets));
    instance->paired_device_count = 0;
}

int mn_approve_pairing(mn_instance *instance,
                       const uint8_t device_id[MN_DEVICE_ID_BYTES],
                       int approved) {
    size_t i;
    if (!instance || !instance->ready ||
        instance->info.role != MN_ROLE_HOST ||
        instance->info.status != MN_STATUS_HOSTING ||
        !instance->encryption_enabled ||
        (approved != 0 && approved != 1) ||
        !mn_device_id_is_valid(device_id))
        return 0;
    for (i = 0; i < MN_MAX_PLAYERS; ++i) {
        mn_peer *peer = &instance->peers[i];
        if (peer->pending_approval && !peer->challenge_pending &&
            memcmp(peer->device_id, device_id, MN_DEVICE_ID_BYTES) == 0) {
            if (approved) {
                if (!mn_pair_device(instance, device_id, peer->device_secret))
                    return 0;
                if (!mn_send_pairing_challenge(instance, peer)) return 0;
            } else {
                uint8_t deny_packet[MN_MAX_DATAGRAM_SIZE];
                size_t deny_size = mn_make_packet(deny_packet, MN_PKT_PAIRING_DENIED,
                                                  instance->config.app_id, 0, NULL, 0);
                mn_send_to(instance, &peer->address, deny_packet, deny_size);
                mn_emit_pairing_event(instance, MN_EVENT_PAIRING_REJECTED,
                                      peer, "Pairing request rejected");
                mn_secure_zero(peer, sizeof(*peer));
            }
            return 1;
        }
    }
    return 0;
}

static void mn_secure_zero(void *memory, size_t size) {
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    while (size--) *bytes++ = 0;
}

int mn_crypto_available(void) {
    return mn_crypto_backend_available();
}

int mn_generate_room_key(uint8_t out_key[MN_ROOM_KEY_BYTES]) {
    if (!out_key || !mn_crypto_backend_available()) return 0;
    return mn_random_bytes(out_key, MN_ROOM_KEY_BYTES);
}

int mn_set_room_key(mn_instance *instance,
                    const uint8_t room_key[MN_ROOM_KEY_BYTES]) {
    size_t i;
    uint8_t nonzero = 0;
    uint64_t prefix;
    if (!instance || !instance->ready || !room_key ||
        !mn_crypto_backend_available() ||
        instance->info.role != MN_ROLE_NONE ||
        instance->info.status != MN_STATUS_OFFLINE)
        return 0;
    for (i = 0; i < MN_ROOM_KEY_BYTES; ++i) nonzero |= room_key[i];
    if (!nonzero || !mn_random_bytes((uint8_t *)&prefix, sizeof(prefix))) return 0;
    memcpy(instance->room_key, room_key, MN_ROOM_KEY_BYTES);
    instance->secure_nonce_prefix = prefix;
    instance->secure_nonce_counter = 0;
    memset(instance->secure_replays, 0, sizeof(instance->secure_replays));
    instance->secure_replay_age = 0;
    instance->encryption_enabled = 1;
    return 1;
}

void mn_clear_room_key(mn_instance *instance) {
    if (!instance || !instance->ready ||
        instance->info.role != MN_ROLE_NONE ||
        instance->info.status != MN_STATUS_OFFLINE)
        return;
    mn_secure_zero(instance->room_key, sizeof(instance->room_key));
    instance->encryption_enabled = 0;
    instance->pairing_required = 0;
    mn_clear_paired_devices(instance);
    instance->secure_nonce_counter = 0;
    memset(instance->secure_replays, 0, sizeof(instance->secure_replays));
    instance->secure_replay_age = 0;
}

void mn_get_info(const mn_instance *instance, mn_info *out_info) {
    if (instance && out_info) *out_info = instance->info;
}

size_t mn_get_players(const mn_instance *instance, mn_player *players, size_t capacity) {
    size_t count;
    if (!instance) return 0;
    count = instance->info.player_count < capacity ? instance->info.player_count : capacity;
    if (players && count) memcpy(players, instance->players, count * sizeof(mn_player));
    return instance->info.player_count;
}

int mn_get_player(const mn_instance *instance, uint32_t player_id, mn_player *out_player) {
    size_t i;
    if (!instance || !out_player || player_id == 0) return 0;
    for (i = 0; i < instance->info.player_count && i < MN_MAX_PLAYERS; ++i) {
        if (instance->players[i].id == player_id) {
            *out_player = instance->players[i];
            return 1;
        }
    }
    return 0;
}

int mn_poll_event(mn_instance *instance, mn_event *out_event) {
    if (!instance || !out_event || instance->event_count == 0) return 0;
    *out_event = instance->events[instance->event_head];
    memset(&instance->events[instance->event_head], 0, sizeof(mn_event));
    instance->event_head = (instance->event_head + 1u) % MN_EVENT_QUEUE_CAPACITY;
    instance->event_count--;
    return 1;
}

int mn_send_message(mn_instance *instance, uint32_t target_player_id,
                    uint16_t channel, const void *data, size_t size) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[20u + MN_MAX_MESSAGE_BYTES];
    size_t packet_size;
    if (!instance || !instance->ready || size > MN_MAX_MESSAGE_BYTES ||
        (size > 0 && !data)) return 0;
    if (instance->info.role == MN_ROLE_HOST && instance->info.status == MN_STATUS_HOSTING) {
        if (target_player_id != 0 && target_player_id != instance->info.local_player_id &&
            !mn_find_peer_id(instance, target_player_id)) return 0;
        mn_emit_message(instance, instance->info.local_player_id, channel,
                        (const uint8_t *)data, size);
        mn_forward_message(instance, instance->info.local_player_id, target_player_id,
                           channel, (const uint8_t *)data, size);
        return 1;
    }
    if (instance->info.role != MN_ROLE_CLIENT ||
        instance->info.status != MN_STATUS_CONNECTED || target_player_id != 0)
        return 0;
    mn_write_u64(payload, instance->session_token);
    mn_write_u32(payload + 8, instance->info.local_player_id);
    mn_write_u32(payload + 12, target_player_id);
    mn_write_u16(payload + 16, channel);
    mn_write_u16(payload + 18, (uint16_t)size);
    if (size) memcpy(payload + 20, data, size);
    packet_size = mn_make_packet(packet, MN_PKT_MESSAGE, instance->config.app_id,
                                 instance->info.room_id, payload, 20u + size);
    mn_emit_message(instance, instance->info.local_player_id, channel,
                    (const uint8_t *)data, size);



    mn_send_to(instance, &instance->server, packet, packet_size);
    return packet_size != 0;
}


int mn_send_reliable_message_ex(mn_instance *instance, uint32_t target_player_id,
                                uint16_t channel, const void *data, size_t size,
                                uint32_t *out_delivery_id) {
    uint32_t message_id;
    size_t i, needed = 0, free_slots = 0;
    if (out_delivery_id) *out_delivery_id = 0;
    if (!instance || !instance->ready || size > MN_MAX_MESSAGE_BYTES ||
        (size > 0 && !data)) return 0;
    if (instance->info.role == MN_ROLE_HOST &&
        instance->info.status == MN_STATUS_HOSTING) {
        if (target_player_id != 0 && target_player_id != instance->info.local_player_id &&
            !mn_find_peer_id(instance, target_player_id)) return 0;
        for (i = 0; i < MN_MAX_PLAYERS; ++i) {
            if (instance->peers[i].active &&
                (target_player_id == 0 || instance->peers[i].id == target_player_id))
                needed++;
        }
        for (i = 0; i < MN_MAX_RELIABLE_PENDING; ++i)
            if (!instance->reliable[i].active) free_slots++;
        if (needed > free_slots) {
            mn_set_error(instance, "Reliable message queue is full");
            return 0;
        }
        instance->next_reliable_id++;
        if (instance->next_reliable_id == 0) instance->next_reliable_id++;
        message_id = instance->next_reliable_id;
        if (out_delivery_id) *out_delivery_id = message_id;
        mn_emit_message(instance, instance->info.local_player_id, channel,
                        (const uint8_t *)data, size);
        for (i = 0; i < MN_MAX_PLAYERS; ++i) {
            mn_peer *peer = &instance->peers[i];
            if (!peer->active ||
                (target_player_id != 0 && peer->id != target_player_id)) continue;
            (void)mn_queue_reliable_for_peer(instance, peer->id,
                                             instance->info.local_player_id,
                                             target_player_id, message_id,
                                             channel, data, size);
        }
        return 1;
    }
    if (instance->info.role != MN_ROLE_CLIENT ||
        instance->info.status != MN_STATUS_CONNECTED || target_player_id != 0) return 0;
    if (!mn_find_reliable_slot(instance)) {
        mn_set_error(instance, "Reliable message queue is full");
        return 0;
    }
    instance->next_reliable_id++;
    if (instance->next_reliable_id == 0) instance->next_reliable_id++;
    message_id = instance->next_reliable_id;
    if (out_delivery_id) *out_delivery_id = message_id;
    if (!mn_queue_reliable_for_peer(instance, 0, instance->info.local_player_id,
                                    0, message_id, channel, data, size)) return 0;
    mn_emit_message(instance, instance->info.local_player_id, channel,
                    (const uint8_t *)data, size);
    return 1;
}

int mn_send_reliable_message(mn_instance *instance, uint32_t target_player_id,
                             uint16_t channel, const void *data, size_t size) {
    return mn_send_reliable_message_ex(instance, target_player_id, channel,
                                       data, size, NULL);
}


void mn_update(mn_instance *instance, float dt_seconds) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    struct sockaddr_in from;
    mn_socklen from_length;
    int received;
    size_t i;
    float dt = dt_seconds;
    if (!instance || !instance->ready || instance->socket == MN_BAD_SOCKET) return;
    if (!mn_float_is_valid(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;

    if (instance->mdns_socket != MN_BAD_SOCKET) {
        for (;;) {
            from_length = (mn_socklen)sizeof(from);
            received = (int)recvfrom(instance->mdns_socket, (char *)packet,
                                     (int)sizeof(packet), 0,
                                     (struct sockaddr *)&from, &from_length);
            if (received <= 0) break;
            if ((size_t)received >= 12u) {
                if (mn_read_u16(packet + 2) & 0x8000u)
                    mn_handle_mdns_response(instance, packet, (size_t)received);
                else
                    mn_handle_mdns_query(instance, packet, (size_t)received, &from);
            }
        }
    }

    for (;;) {
        from_length = (mn_socklen)sizeof(from);
        received = (int)recvfrom(instance->socket, (char *)packet,
                                 (int)sizeof(packet), 0,
                                 (struct sockaddr *)&from, &from_length);
        if (received <= 0) break;
        mn_handle_packet(instance, packet, (size_t)received, &from);
    }

    if (instance->info.status == MN_STATUS_SEARCHING) {
        instance->discovery_left -= dt;
        instance->discovery_tick -= dt;
        if (instance->discovery_left > 0.0f && instance->discovery_tick <= 0.0f) {
            if (instance->discovery_auto)
                mn_send_auto_discover(instance);
            else if (instance->discovery_mdns)
                mn_send_mdns_query(instance);
            else if (instance->discovery_target_valid)
                mn_send_discover_to(instance, &instance->discovery_target);
            else
                mn_send_discover(instance);
            instance->discovery_tick = 0.5f;
        }
        if (instance->discovery_left <= 0.0f) {
            instance->info.status = MN_STATUS_OFFLINE;
            instance->info.role = MN_ROLE_NONE;
            instance->discovery_target_valid = 0;
            instance->discovery_mdns = 0;
            instance->discovery_auto = 0;
            memset(&instance->discovery_target, 0, sizeof(instance->discovery_target));
        }
    }

    if (instance->info.role == MN_ROLE_CLIENT &&
        instance->info.status == MN_STATUS_CONNECTING) {
        instance->connect_elapsed += dt;
        if (instance->connect_elapsed > MN_CONNECT_TIMEOUT) {
            instance->info.role = MN_ROLE_NONE;
            instance->info.status = MN_STATUS_OFFLINE;
            mn_set_error(instance, "Connection timed out");
        } else {
            instance->heartbeat_tick -= dt;
            if (instance->heartbeat_tick <= 0.0f) {
                mn_send_join(instance);
                instance->heartbeat_tick = 0.5f;
            }
        }
    } else if (instance->info.role == MN_ROLE_CLIENT &&
               instance->info.status == MN_STATUS_CONNECTED) {
        instance->server_silence += dt;
        if (instance->server_silence > instance->config.timeout_seconds) {
            uint32_t player_id = instance->info.local_player_id;
            instance->info.role = MN_ROLE_NONE;
            instance->info.status = MN_STATUS_OFFLINE;
            instance->info.player_count = 0;
            instance->info.match_started = 0;
            instance->info.ping_pending = 0;
            instance->info.ping_available = 0;
            instance->ping_elapsed = 0.0f;
            instance->ping_pending_id = 0;
            instance->ping_pending_target = 0;
            memset(instance->players, 0, sizeof(instance->players));
            mn_emit_simple_event(instance, MN_EVENT_DISCONNECTED,
                                 "Host disconnected", player_id);
            mn_set_error(instance, "Host disconnected");
        } else {
            instance->heartbeat_tick -= dt;
            if (instance->heartbeat_tick <= 0.0f) {
                mn_send_state(instance);
                instance->heartbeat_tick = instance->config.heartbeat_seconds;
            }



        }
    }

    if (instance->info.ping_pending) {
        instance->ping_elapsed += dt;
        if (instance->ping_elapsed >= MN_PING_TIMEOUT_SECONDS) {
            instance->info.ping_pending = 0;
            instance->info.ping_available = 0;
            instance->ping_pending_id = 0;
            instance->ping_pending_target = 0;
            instance->ping_elapsed = 0.0f;
            mn_emit_simple_event(instance, MN_EVENT_ERROR, "Ping request timed out", 0);
        }
    }

    for (i = 0; i < MN_MAX_RELIABLE_PENDING; ++i) {
        mn_reliable_entry *entry = &instance->reliable[i];
        if (!entry->active) continue;
        entry->retry_elapsed += dt;
        if (entry->retry_elapsed >= MN_RELIABLE_RETRY_SECONDS) {
            if (entry->attempts >= MN_RELIABLE_MAX_ATTEMPTS) {
                char error_text[160];
                (void)snprintf(error_text, sizeof(error_text),
                               "Reliable message %u to player %u was not acknowledged",
                               (unsigned)entry->message_id, (unsigned)entry->peer_id);
                mn_emit_reliable_status(instance, MN_EVENT_MESSAGE_FAILED,
                                         entry, entry->peer_id, error_text);
                mn_copy_text(instance->info.last_error, sizeof(instance->info.last_error),
                             error_text);
                mn_emit_simple_event(instance, MN_EVENT_ERROR, error_text, entry->peer_id);
                memset(entry, 0, sizeof(*entry));
            } else {
                entry->attempts++;
                entry->retry_elapsed = 0.0f;
                mn_send_reliable_entry(instance, entry);
            }
        }
    }

    if (instance->info.role == MN_ROLE_HOST) {
        for (i = 0; i < MN_MAX_PLAYERS; ++i) {
            mn_peer *peer = &instance->peers[i];
            if (peer->active || peer->pending_approval) {
                peer->silence += dt;
                if (peer->active) {
                    peer->state_cooldown -= dt;
                    peer->message_cooldown -= dt;
                    if (peer->state_cooldown < 0.0f)
                        peer->state_cooldown = 0.0f;
                    if (peer->message_cooldown < 0.0f)
                        peer->message_cooldown = 0.0f;
                }
                if (peer->silence > instance->config.timeout_seconds) {
                    if (peer->active) {
                        mn_remove_peer(instance, peer, "Player timed out");
                        mn_broadcast_snapshot(instance);
                    } else {
                        memset(peer, 0, sizeof(*peer));
                    }
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


int mn_ping(mn_instance *instance, uint32_t target_player_id) {
    uint8_t packet[MN_MAX_DATAGRAM_SIZE];
    uint8_t payload[12];
    size_t payload_size, packet_size;
    const struct sockaddr_in *destination = NULL;
    mn_peer *peer = NULL;
    if (!instance || !instance->ready || instance->info.ping_pending) return 0;
    if (instance->info.role == MN_ROLE_HOST &&
        instance->info.status == MN_STATUS_HOSTING && target_player_id != 0) {
        peer = mn_find_peer_id(instance, target_player_id);
        if (!peer) return 0;
        destination = &peer->address;
        mn_write_u32(payload, 0);
        payload_size = 4u;
    } else if (instance->info.role == MN_ROLE_CLIENT &&
               instance->info.status == MN_STATUS_CONNECTED && target_player_id == 0) {
        mn_write_u64(payload, instance->session_token);
        mn_write_u32(payload + 8, 0);
        payload_size = 12u;
        destination = &instance->server;
    } else {
        return 0;
    }
    instance->ping_sequence++;
    if (instance->ping_sequence == 0) instance->ping_sequence++;
    if (instance->info.role == MN_ROLE_CLIENT) {
        mn_write_u32(payload + 8, instance->ping_sequence);
    } else {
        mn_write_u32(payload, instance->ping_sequence);
    }
    packet_size = mn_make_packet(packet, MN_PKT_PING, instance->config.app_id,
                                 instance->info.room_id, payload, payload_size);
    mn_send_to(instance, destination, packet, packet_size);
    instance->ping_pending_id = instance->ping_sequence;
    instance->ping_pending_target = target_player_id;
    instance->ping_elapsed = 0.0f;
    instance->info.ping_ms = 0.0f;
    instance->info.ping_available = 0;
    instance->info.ping_pending = 1;
    return packet_size != 0;
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
