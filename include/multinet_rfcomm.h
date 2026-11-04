/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#ifndef MULTINET_RFCOMM_H
#define MULTINET_RFCOMM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_RFCOMM_NAME_LENGTH 96u
#define MN_RFCOMM_MAX_DEVICES 32u

typedef struct mn_rfcomm mn_rfcomm;

typedef enum mn_rfcomm_state {
    MN_RFCOMM_IDLE = 0,
    MN_RFCOMM_LISTENING = 1,
    MN_RFCOMM_CONNECTING = 2,
    MN_RFCOMM_CONNECTED = 3
} mn_rfcomm_state;

/* A remote service returned by Bluetooth SDP discovery. The address is the
   48-bit Bluetooth device address stored in a uint64_t; channel is RFCOMM 1-30. */
typedef struct mn_rfcomm_device {
    uint64_t address;
    uint32_t channel;
    char name[MN_RFCOMM_NAME_LENGTH];
} mn_rfcomm_device;

typedef struct mn_rfcomm_info {
    mn_rfcomm_state state;
    uint32_t local_channel;
    uint64_t remote_address;
    char service_name[MN_RFCOMM_NAME_LENGTH];
    char last_error[160];
} mn_rfcomm_info;

/* Native RFCOMM is currently implemented with Windows AF_BTH sockets.
   On other platforms these functions remain linkable but report unsupported. */
int mn_rfcomm_supported(void);
mn_rfcomm *mn_rfcomm_create(void);
void mn_rfcomm_destroy(mn_rfcomm *connection);

/* Advertise one RFCOMM service and listen for a single peer. Windows pairing
   and passkey UI are handled by the operating system. When secure_link is
   non-zero, authentication and link encryption are both required. */
int mn_rfcomm_host(mn_rfcomm *connection, const char *service_name,
                   int secure_link);

/* Synchronously inquire nearby Bluetooth devices and query each for the
   MultiNet RFCOMM service. SDP inquiry can take several seconds. Returns the
   number of matching services; copies up to capacity results when devices is
   non-NULL. */
size_t mn_rfcomm_discover(mn_rfcomm *connection,
                          mn_rfcomm_device *devices, size_t capacity);

/* Connect to a service returned by mn_rfcomm_discover(). The connection is
   non-blocking; call mn_rfcomm_update() until state becomes CONNECTED. */
int mn_rfcomm_join(mn_rfcomm *connection, const mn_rfcomm_device *device,
                   int secure_link);

/* Poll listener/connect state. Call regularly from the owning UI/game thread. */
int mn_rfcomm_update(mn_rfcomm *connection);

/* Non-blocking byte-stream I/O. read/write may transfer fewer bytes than
   requested; a successful call with *out_transferred == 0 means would-block.
   Applications must provide their own framing for messages. */
int mn_rfcomm_read(mn_rfcomm *connection, void *buffer, size_t capacity,
                   size_t *out_transferred);
int mn_rfcomm_write(mn_rfcomm *connection, const void *data, size_t size,
                    size_t *out_transferred);

void mn_rfcomm_close(mn_rfcomm *connection);
void mn_rfcomm_get_info(const mn_rfcomm *connection, mn_rfcomm_info *out_info);

#ifdef __cplusplus
}
#endif
#endif /* MULTINET_RFCOMM_H */
