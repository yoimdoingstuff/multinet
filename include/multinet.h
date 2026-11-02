/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#ifndef MULTINET_H
#define MULTINET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_VERSION_MAJOR 0
#define MN_VERSION_MINOR 1
#define MN_VERSION_PATCH 0
#define MN_VERSION_STRING "0.1.0"

#define MN_MAX_PLAYERS 16u
#define MN_MAX_ROOMS 32u
#define MN_PLAYER_NAME_LENGTH 24u
#define MN_ROOM_NAME_LENGTH 32u
#define MN_MAX_MESSAGE_BYTES 768u
#define MN_DEFAULT_PORT 27890u
#define MN_DEFAULT_APP_ID 0x4D4E5431u /* "MNT1" */

#include "multinet_types.inc"
#include "multinet_api.inc"

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* MULTINET_H */