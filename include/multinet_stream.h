/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#ifndef MULTINET_STREAM_H
#define MULTINET_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_STREAM_FRAME_HEADER_BYTES 12u
#define MN_STREAM_MAX_MESSAGE_BYTES 65536u
#define MN_STREAM_MAX_PENDING_SENDS 32u
#define MN_STREAM_MAX_PENDING_MESSAGES 32u
#define MN_STREAM_MAX_QUEUED_BYTES (1024u * 1024u)

typedef struct mn_stream mn_stream;
typedef struct mn_rfcomm mn_rfcomm;

/* Caller-owned byte-stream adapter. Callbacks must be non-blocking, must
   return 1 on success, and set *out_transferred to zero when they would block.
   The adapter and its context must outlive the mn_stream object. */
typedef struct mn_stream_transport {
    void *context;
    int (*update)(void *context);
    int (*is_connected)(void *context);
    int (*read)(void *context, void *buffer, size_t capacity,
                size_t *out_transferred);
    int (*write)(void *context, const void *data, size_t size,
                 size_t *out_transferred);
} mn_stream_transport;

/* A transport-independent framed message channel layered over any
   non-blocking byte stream. The stream borrows the transport; destroying it
   never closes or frees the underlying transport. */
mn_stream *mn_stream_create(const mn_stream_transport *transport,
                            uint32_t max_message_bytes);
void mn_stream_destroy(mn_stream *stream);

/* Queue one message. The queue is bounded; call mn_stream_update() to make
   progress and retry when this returns zero because the queue is full. */
int mn_stream_send(mn_stream *stream, uint16_t channel,
                   const void *data, size_t size);

/* Pump the transport and move complete messages into the receive queue.
   Returns zero on a transport/protocol error, otherwise one. */
int mn_stream_update(mn_stream *stream);

/* Returns 1 when a message was copied and consumed, 0 when none is queued,
   or -1 when the caller's buffer is too small. On -1, out_size is set to the
   required size and the message remains queued for a later poll. */
int mn_stream_poll(mn_stream *stream, uint16_t *out_channel,
                   void *buffer, size_t capacity, size_t *out_size);

const char *mn_stream_last_error(const mn_stream *stream);
size_t mn_stream_pending_send_bytes(const mn_stream *stream);
size_t mn_stream_pending_messages(const mn_stream *stream);

/* Convenience adapter for the experimental Windows RFCOMM backend. The
   returned transport borrows connection and is valid until it is closed or
   destroyed. Unsupported platforms return zero. */
int mn_stream_transport_from_rfcomm(mn_rfcomm *connection,
                                    mn_stream_transport *out_transport);

#ifdef __cplusplus
}
#endif
#endif /* MULTINET_STREAM_H */
