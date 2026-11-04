/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet_stream.h"
#include "multinet_rfcomm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MN_STREAM_MAGIC 0x4D4E5331u /* "MNS1" */
#define MN_STREAM_VERSION 1u
#define MN_STREAM_READ_CHUNK 4096u
#define MN_STREAM_READ_BUDGET (16u * MN_STREAM_READ_CHUNK)
#define MN_STREAM_WRITE_BUDGET 32u

typedef struct mn_stream_tx_entry {
    uint8_t *frame;
    size_t size;
    size_t offset;
} mn_stream_tx_entry;

typedef struct mn_stream_rx_entry {
    uint16_t channel;
    uint32_t size;
    uint8_t *data;
} mn_stream_rx_entry;

struct mn_stream {
    mn_stream_transport transport;
    uint32_t max_message_bytes;
    int failed;
    int was_connected;
    char last_error[160];

    mn_stream_tx_entry tx[MN_STREAM_MAX_PENDING_SENDS];
    size_t tx_head;
    size_t tx_count;
    size_t tx_bytes;

    uint8_t rx_header[MN_STREAM_FRAME_HEADER_BYTES];
    size_t rx_header_used;
    uint16_t rx_channel;
    uint32_t rx_expected;
    uint8_t *rx_payload;
    size_t rx_payload_used;

    mn_stream_rx_entry messages[MN_STREAM_MAX_PENDING_MESSAGES];
    size_t message_head;
    size_t message_count;
    size_t message_bytes;
};

static void mn_stream_write_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void mn_stream_write_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint16_t mn_stream_read_u16(const uint8_t *in) {
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

static uint32_t mn_stream_read_u32(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static int mn_stream_fail(mn_stream *stream, const char *message) {
    if (!stream) return 0;
    if (!stream->failed) {
        stream->failed = 1;
        (void)snprintf(stream->last_error, sizeof(stream->last_error), "%s",
                       message ? message : "Framed stream failed");
    }
    free(stream->rx_payload);
    stream->rx_payload = NULL;
    stream->rx_expected = 0;
    stream->rx_payload_used = 0;
    return 0;
}

static void mn_stream_release_queues(mn_stream *stream) {
    size_t i;
    if (!stream) return;
    for (i = 0; i < MN_STREAM_MAX_PENDING_SENDS; ++i) {
        free(stream->tx[i].frame);
        memset(&stream->tx[i], 0, sizeof(stream->tx[i]));
    }
    for (i = 0; i < MN_STREAM_MAX_PENDING_MESSAGES; ++i) {
        free(stream->messages[i].data);
        memset(&stream->messages[i], 0, sizeof(stream->messages[i]));
    }
    free(stream->rx_payload);
    stream->rx_payload = NULL;
}

mn_stream *mn_stream_create(const mn_stream_transport *transport,
                            uint32_t max_message_bytes) {
    mn_stream *stream;
    if (!transport || !transport->update || !transport->is_connected ||
        !transport->read || !transport->write)
        return NULL;
    if (max_message_bytes == 0)
        max_message_bytes = MN_STREAM_MAX_MESSAGE_BYTES;
    if (max_message_bytes > MN_STREAM_MAX_MESSAGE_BYTES)
        return NULL;
    stream = (mn_stream *)calloc(1u, sizeof(*stream));
    if (!stream) return NULL;
    stream->transport = *transport;
    stream->max_message_bytes = max_message_bytes;
    return stream;
}

void mn_stream_destroy(mn_stream *stream) {
    if (!stream) return;
    mn_stream_release_queues(stream);
    free(stream);
}

void mn_stream_reset(mn_stream *stream) {
    if (!stream) return;
    mn_stream_release_queues(stream);
    stream->tx_head = 0;
    stream->tx_count = 0;
    stream->tx_bytes = 0;
    memset(stream->rx_header, 0, sizeof(stream->rx_header));
    stream->rx_header_used = 0;
    stream->rx_channel = 0;
    stream->rx_expected = 0;
    stream->rx_payload_used = 0;
    stream->message_head = 0;
    stream->message_count = 0;
    stream->message_bytes = 0;
    stream->failed = 0;
    stream->was_connected =
        stream->transport.is_connected(stream->transport.context) ? 1 : 0;
    stream->last_error[0] = '\0';
}

int mn_stream_send(mn_stream *stream, uint16_t channel,
                   const void *data, size_t size) {
    size_t frame_size;
    size_t tail;
    uint8_t *frame;
    if (!stream || stream->failed || (!data && size != 0)) return 0;
    if (size > stream->max_message_bytes || size > UINT32_MAX)
        return 0;
    if (stream->tx_count >= MN_STREAM_MAX_PENDING_SENDS)
        return 0;
    frame_size = MN_STREAM_FRAME_HEADER_BYTES + size;
    if (frame_size > MN_STREAM_MAX_QUEUED_BYTES ||
        stream->tx_bytes > MN_STREAM_MAX_QUEUED_BYTES - frame_size)
        return 0;
    frame = (uint8_t *)malloc(frame_size);
    if (!frame) return 0;
    mn_stream_write_u32(frame, MN_STREAM_MAGIC);
    frame[4] = MN_STREAM_VERSION;
    frame[5] = 0u;
    mn_stream_write_u16(frame + 6u, channel);
    mn_stream_write_u32(frame + 8u, (uint32_t)size);
    if (size != 0) memcpy(frame + MN_STREAM_FRAME_HEADER_BYTES, data, size);
    tail = (stream->tx_head + stream->tx_count) % MN_STREAM_MAX_PENDING_SENDS;
    stream->tx[tail].frame = frame;
    stream->tx[tail].size = frame_size;
    stream->tx[tail].offset = 0;
    stream->tx_count++;
    stream->tx_bytes += frame_size;
    return 1;
}

static int mn_stream_queue_received(mn_stream *stream) {
    size_t tail;
    if (stream->message_count >= MN_STREAM_MAX_PENDING_MESSAGES)
        return mn_stream_fail(stream, "Incoming message queue is full");
    if ((size_t)stream->rx_expected > MN_STREAM_MAX_QUEUED_BYTES ||
        stream->message_bytes > MN_STREAM_MAX_QUEUED_BYTES -
                               (size_t)stream->rx_expected)
        return mn_stream_fail(stream, "Incoming message queue byte limit exceeded");
    tail = (stream->message_head + stream->message_count) %
           MN_STREAM_MAX_PENDING_MESSAGES;
    stream->messages[tail].channel = stream->rx_channel;
    stream->messages[tail].size = stream->rx_expected;
    stream->messages[tail].data = stream->rx_payload;
    stream->message_count++;
    stream->message_bytes += (size_t)stream->rx_expected;
    stream->rx_payload = NULL;
    stream->rx_header_used = 0;
    stream->rx_channel = 0;
    stream->rx_expected = 0;
    stream->rx_payload_used = 0;
    return 1;
}

static int mn_stream_parse_bytes(mn_stream *stream,
                                 const uint8_t *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        if (stream->rx_header_used < MN_STREAM_FRAME_HEADER_BYTES) {
            size_t needed = MN_STREAM_FRAME_HEADER_BYTES - stream->rx_header_used;
            size_t available = size - offset;
            size_t copy_size = needed < available ? needed : available;
            memcpy(stream->rx_header + stream->rx_header_used,
                   data + offset, copy_size);
            stream->rx_header_used += copy_size;
            offset += copy_size;
            if (stream->rx_header_used < MN_STREAM_FRAME_HEADER_BYTES)
                continue;

            if (mn_stream_read_u32(stream->rx_header) != MN_STREAM_MAGIC ||
                stream->rx_header[4] != MN_STREAM_VERSION ||
                stream->rx_header[5] != 0u)
                return mn_stream_fail(stream, "Invalid framed stream header");
            stream->rx_channel = mn_stream_read_u16(stream->rx_header + 6u);
            stream->rx_expected = mn_stream_read_u32(stream->rx_header + 8u);
            if (stream->rx_expected > stream->max_message_bytes)
                return mn_stream_fail(stream, "Incoming message exceeds the configured stream limit");
            if (stream->rx_expected == 0u) {
                if (!mn_stream_queue_received(stream)) return 0;
                continue;
            }
            stream->rx_payload = (uint8_t *)malloc((size_t)stream->rx_expected);
            if (!stream->rx_payload)
                return mn_stream_fail(stream, "Could not allocate an incoming message");
        }

        if (stream->rx_expected > 0u) {
            size_t needed = (size_t)stream->rx_expected - stream->rx_payload_used;
            size_t available = size - offset;
            size_t copy_size = needed < available ? needed : available;
            memcpy(stream->rx_payload + stream->rx_payload_used,
                   data + offset, copy_size);
            stream->rx_payload_used += copy_size;
            offset += copy_size;
            if (stream->rx_payload_used == (size_t)stream->rx_expected) {
                if (!mn_stream_queue_received(stream)) return 0;
            }
        }
    }
    return 1;
}

static int mn_stream_flush_writes(mn_stream *stream) {
    size_t attempt;
    for (attempt = 0; attempt < MN_STREAM_WRITE_BUDGET && stream->tx_count > 0;
         ++attempt) {
        mn_stream_tx_entry *entry = &stream->tx[stream->tx_head];
        size_t transferred = 0;
        size_t remaining = entry->size - entry->offset;
        if (!stream->transport.write(stream->transport.context,
                entry->frame + entry->offset, remaining, &transferred))
            return mn_stream_fail(stream, "Underlying stream write failed");
        if (transferred > remaining)
            return mn_stream_fail(stream, "Underlying stream reported an invalid write length");
        if (transferred == 0) break;
        entry->offset += transferred;
        stream->tx_bytes -= transferred;
        if (entry->offset == entry->size) {
            free(entry->frame);
            memset(entry, 0, sizeof(*entry));
            stream->tx_head = (stream->tx_head + 1u) % MN_STREAM_MAX_PENDING_SENDS;
            stream->tx_count--;
        }
    }
    return 1;
}

static int mn_stream_read_available(mn_stream *stream) {
    uint8_t buffer[MN_STREAM_READ_CHUNK];
    size_t total_read = 0;
    while (total_read < MN_STREAM_READ_BUDGET) {
        size_t transferred = 0;
        size_t capacity = sizeof(buffer);
        if (!stream->transport.read(stream->transport.context,
                                    buffer, capacity, &transferred))
            return mn_stream_fail(stream, "Underlying stream read failed");
        if (transferred > capacity)
            return mn_stream_fail(stream, "Underlying stream reported an invalid read length");
        if (transferred == 0) break;
        if (!mn_stream_parse_bytes(stream, buffer, transferred)) return 0;
        total_read += transferred;
    }
    return 1;
}

int mn_stream_update(mn_stream *stream) {
    int connected;
    if (!stream || stream->failed) return 0;
    if (!stream->transport.update(stream->transport.context))
        return mn_stream_fail(stream, "Underlying transport update failed");
    connected = stream->transport.is_connected(stream->transport.context);
    if (!connected) {
        if (stream->was_connected)
            return mn_stream_fail(stream, "Underlying stream disconnected");
        return 1;
    }
    stream->was_connected = 1;
    if (!mn_stream_flush_writes(stream)) return 0;
    if (!mn_stream_read_available(stream)) return 0;
    return 1;
}

int mn_stream_poll(mn_stream *stream, uint16_t *out_channel,
                   void *buffer, size_t capacity, size_t *out_size) {
    mn_stream_rx_entry *entry;
    if (out_size) *out_size = 0;
    if (!stream || stream->message_count == 0) return 0;
    entry = &stream->messages[stream->message_head];
    if (out_size) *out_size = (size_t)entry->size;
    if (capacity < (size_t)entry->size || (!buffer && entry->size != 0))
        return -1;
    if (entry->size != 0)
        memcpy(buffer, entry->data, (size_t)entry->size);
    if (out_channel) *out_channel = entry->channel;
    stream->message_bytes -= (size_t)entry->size;
    free(entry->data);
    memset(entry, 0, sizeof(*entry));
    stream->message_head = (stream->message_head + 1u) %
                           MN_STREAM_MAX_PENDING_MESSAGES;
    stream->message_count--;
    return 1;
}

const char *mn_stream_last_error(const mn_stream *stream) {
    if (!stream) return "Invalid framed stream";
    return stream->last_error;
}

size_t mn_stream_pending_send_bytes(const mn_stream *stream) {
    return stream ? stream->tx_bytes : 0u;
}

size_t mn_stream_pending_messages(const mn_stream *stream) {
    return stream ? stream->message_count : 0u;
}

static int mn_rfcomm_transport_update(void *context) {
    return mn_rfcomm_update((mn_rfcomm *)context);
}

static int mn_rfcomm_transport_connected(void *context) {
    mn_rfcomm_info info;
    mn_rfcomm_get_info((const mn_rfcomm *)context, &info);
    return info.state == MN_RFCOMM_CONNECTED;
}

static int mn_rfcomm_transport_read(void *context, void *buffer,
                                    size_t capacity, size_t *out_transferred) {
    return mn_rfcomm_read((mn_rfcomm *)context, buffer, capacity,
                          out_transferred);
}

static int mn_rfcomm_transport_write(void *context, const void *data,
                                     size_t size, size_t *out_transferred) {
    return mn_rfcomm_write((mn_rfcomm *)context, data, size,
                           out_transferred);
}

int mn_stream_transport_from_rfcomm(mn_rfcomm *connection,
                                    mn_stream_transport *out_transport) {
    if (!connection || !out_transport || !mn_rfcomm_supported()) return 0;
    memset(out_transport, 0, sizeof(*out_transport));
    out_transport->context = connection;
    out_transport->update = mn_rfcomm_transport_update;
    out_transport->is_connected = mn_rfcomm_transport_connected;
    out_transport->read = mn_rfcomm_transport_read;
    out_transport->write = mn_rfcomm_transport_write;
    return 1;
}
