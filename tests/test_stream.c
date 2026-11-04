/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PIPE_CAPACITY 131072u

typedef struct test_pipe {
    uint8_t bytes[TEST_PIPE_CAPACITY];
    size_t head;
    size_t count;
} test_pipe;

typedef struct test_wire {
    test_pipe a_to_b;
    test_pipe b_to_a;
} test_wire;

typedef struct test_endpoint {
    test_wire *wire;
    int side;
    size_t max_read;
    size_t max_write;
} test_endpoint;

static int failures;

#define CHECK(condition, description) do { \
    if (condition) { \
        printf("[PASS] %s\n", description); \
    } else { \
        fprintf(stderr, "[FAIL] %s\n", description); \
        failures++; \
    } \
} while (0)

static int pipe_write(test_pipe *pipe, const void *data, size_t size,
                      size_t maximum, size_t *out_written) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t available = TEST_PIPE_CAPACITY - pipe->count;
    size_t amount = size < maximum ? size : maximum;
    size_t i;
    if (amount > available) amount = available;
    for (i = 0; i < amount; ++i) {
        size_t tail = (pipe->head + pipe->count + i) % TEST_PIPE_CAPACITY;
        pipe->bytes[tail] = bytes[i];
    }
    pipe->count += amount;
    if (out_written) *out_written = amount;
    return 1;
}

static int pipe_read(test_pipe *pipe, void *data, size_t capacity,
                     size_t maximum, size_t *out_read) {
    uint8_t *bytes = (uint8_t *)data;
    size_t amount = pipe->count < capacity ? pipe->count : capacity;
    size_t i;
    if (amount > maximum) amount = maximum;
    for (i = 0; i < amount; ++i)
        bytes[i] = pipe->bytes[(pipe->head + i) % TEST_PIPE_CAPACITY];
    pipe->head = (pipe->head + amount) % TEST_PIPE_CAPACITY;
    pipe->count -= amount;
    if (out_read) *out_read = amount;
    return 1;
}

static int fake_update(void *context) {
    return context != NULL;
}

static int fake_connected(void *context) {
    return context != NULL;
}

static int fake_read(void *context, void *buffer, size_t capacity,
                     size_t *out_transferred) {
    test_endpoint *endpoint = (test_endpoint *)context;
    test_pipe *incoming;
    if (!endpoint || !endpoint->wire) return 0;
    incoming = endpoint->side == 0
        ? &endpoint->wire->b_to_a : &endpoint->wire->a_to_b;
    return pipe_read(incoming, buffer, capacity, endpoint->max_read,
                     out_transferred);
}

static int fake_write(void *context, const void *data, size_t size,
                      size_t *out_transferred) {
    test_endpoint *endpoint = (test_endpoint *)context;
    test_pipe *outgoing;
    if (!endpoint || !endpoint->wire) return 0;
    outgoing = endpoint->side == 0
        ? &endpoint->wire->a_to_b : &endpoint->wire->b_to_a;
    return pipe_write(outgoing, data, size, endpoint->max_write,
                      out_transferred);
}

static mn_stream_transport make_transport(test_endpoint *endpoint) {
    mn_stream_transport transport;
    memset(&transport, 0, sizeof(transport));
    transport.context = endpoint;
    transport.update = fake_update;
    transport.is_connected = fake_connected;
    transport.read = fake_read;
    transport.write = fake_write;
    return transport;
}

static void pump_pair(mn_stream *a, mn_stream *b, unsigned rounds) {
    unsigned i;
    for (i = 0; i < rounds; ++i) {
        if (!mn_stream_update(a)) break;
        if (!mn_stream_update(b)) break;
    }
}

static void inject_bytes(test_pipe *pipe, const uint8_t *bytes, size_t size) {
    size_t written = 0;
    (void)pipe_write(pipe, bytes, size, size, &written);
}

int main(void) {
    test_wire wire;
    test_endpoint endpoint_a, endpoint_b;
    mn_stream_transport transport_a, transport_b;
    mn_stream *stream_a, *stream_b;
    const char first[] = "simultaneous messages survive partial writes";
    const char second[] = "reply from B";
    char received[256];
    size_t received_size;
    uint16_t channel;
    unsigned i;
    int poll_result;

    memset(&wire, 0, sizeof(wire));
    memset(&endpoint_a, 0, sizeof(endpoint_a));
    memset(&endpoint_b, 0, sizeof(endpoint_b));
    endpoint_a.wire = &wire;
    endpoint_a.side = 0;
    endpoint_a.max_read = 2u;
    endpoint_a.max_write = 3u;
    endpoint_b.wire = &wire;
    endpoint_b.side = 1;
    endpoint_b.max_read = 2u;
    endpoint_b.max_write = 3u;
    transport_a = make_transport(&endpoint_a);
    transport_b = make_transport(&endpoint_b);
    stream_a = mn_stream_create(&transport_a, 128u);
    stream_b = mn_stream_create(&transport_b, 128u);
    CHECK(stream_a != NULL && stream_b != NULL,
          "create framed endpoints over arbitrary byte-stream adapters");
    if (!stream_a || !stream_b) goto cleanup;

    CHECK(mn_stream_send(stream_a, 7u, first, sizeof(first) - 1u),
          "queue first framed message");
    CHECK(mn_stream_send(stream_a, 8u, NULL, 0u),
          "queue zero-length framed message");
    CHECK(mn_stream_send(stream_b, 9u, second, sizeof(second) - 1u),
          "queue simultaneous message in the opposite direction");
    pump_pair(stream_a, stream_b, 120u);
    CHECK(mn_stream_pending_send_bytes(stream_a) == 0u &&
          mn_stream_pending_send_bytes(stream_b) == 0u,
          "flush all queued data despite partial writes");

    memset(received, 0, sizeof(received));
    poll_result = mn_stream_poll(stream_b, &channel, received, 4u,
                                 &received_size);
    CHECK(poll_result == -1 && received_size == sizeof(first) - 1u,
          "report required buffer size without consuming a message");
    poll_result = mn_stream_poll(stream_b, &channel, received,
                                 sizeof(received), &received_size);
    CHECK(poll_result == 1 && channel == 7u &&
          received_size == sizeof(first) - 1u &&
          memcmp(received, first, received_size) == 0,
          "reassemble a fragmented frame and preserve its channel and payload");
    poll_result = mn_stream_poll(stream_b, &channel, NULL, 0u,
                                 &received_size);
    CHECK(poll_result == 1 && channel == 8u && received_size == 0u,
          "deliver an empty message without a payload buffer");

    poll_result = mn_stream_poll(stream_a, &channel, received,
                                 sizeof(received), &received_size);
    CHECK(poll_result == 1 && channel == 9u &&
          received_size == sizeof(second) - 1u &&
          memcmp(received, second, received_size) == 0,
          "deliver simultaneous messages in the reverse direction");

    for (i = 0; i < MN_STREAM_MAX_PENDING_SENDS; ++i)
        CHECK(mn_stream_send(stream_a, (uint16_t)i, NULL, 0u),
              "queue a bounded empty message");
    CHECK(!mn_stream_send(stream_a, 999u, NULL, 0u),
          "reject a message when the bounded send queue is full");
    CHECK(mn_stream_update(stream_a),
          "a full application queue does not poison the stream");
    pump_pair(stream_a, stream_b, 20u);
    CHECK(mn_stream_pending_messages(stream_b) == MN_STREAM_MAX_PENDING_MESSAGES,
          "receive queue can hold its documented message limit");
    for (i = 0; i < MN_STREAM_MAX_PENDING_MESSAGES; ++i) {
        poll_result = mn_stream_poll(stream_b, &channel, NULL, 0u, &received_size);
        CHECK(poll_result == 1 && received_size == 0u,
              "poll one message from the bounded receive queue");
    }

    mn_stream_destroy(stream_b);
    stream_b = NULL;
    {
        const uint8_t invalid_header[MN_STREAM_FRAME_HEADER_BYTES] = {
            0u, 0u, 0u, 0u, 1u, 0u, 0u, 1u, 0u, 0u, 0u, 0u
        };
        inject_bytes(&wire.a_to_b, invalid_header, sizeof(invalid_header));
        stream_b = mn_stream_create(&transport_b, 128u);
        CHECK(stream_b != NULL, "recreate receiver for malformed-frame test");
        CHECK(stream_b != NULL && !mn_stream_update(stream_b),
              "reject a frame with an invalid magic value");
        CHECK(stream_b != NULL &&
              strstr(mn_stream_last_error(stream_b), "header") != NULL,
              "expose a useful malformed-header error");
    }

    mn_stream_destroy(stream_b);
    stream_b = NULL;
    memset(&wire, 0, sizeof(wire));
    {
        const uint8_t oversized_header[MN_STREAM_FRAME_HEADER_BYTES] = {
            0x4Du, 0x4Eu, 0x53u, 0x31u, 1u, 0u, 0u, 1u,
            0u, 0u, 0u, 129u
        };
        inject_bytes(&wire.a_to_b, oversized_header, sizeof(oversized_header));
        stream_b = mn_stream_create(&transport_b, 128u);
        CHECK(stream_b != NULL && !mn_stream_update(stream_b),
              "reject a peer-declared message larger than the configured limit");
        CHECK(stream_b != NULL &&
              strstr(mn_stream_last_error(stream_b), "limit") != NULL,
              "report oversized incoming messages clearly");
    }

cleanup:
    if (stream_a) mn_stream_destroy(stream_a);
    if (stream_b) mn_stream_destroy(stream_b);
    if (failures != 0) {
        fprintf(stderr, "%d framed-stream test(s) failed.\n", failures);
        return 1;
    }
    puts("MultiNet framed-stream tests passed.");
    return 0;
}
