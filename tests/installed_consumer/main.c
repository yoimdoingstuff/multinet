#include <stdio.h>
#include "multinet.h"
#include "multinet_stream.h"

static int empty_update(void *context) { return context != NULL; }
static int empty_connected(void *context) { (void)context; return 0; }
static int empty_read(void *context, void *buffer, size_t capacity, size_t *out_size) {
    (void)context; (void)buffer; (void)capacity;
    if (out_size) *out_size = 0;
    return 1;
}
static int empty_write(void *context, const void *data, size_t size, size_t *out_size) {
    (void)context; (void)data; (void)size;
    if (out_size) *out_size = 0;
    return 1;
}

int main(void) {
    mn_config config;
    mn_instance *network;
    mn_stream_transport transport;
    mn_stream *stream;
    int adapter_context = 1;

    mn_config_init(&config);
    config.enable_broadcast = 0;
    network = mn_create(&config);
    if (!network) {
        fputs("MultiNet consumer could not create an instance.\n", stderr);
        return 1;
    }

    transport.context = &adapter_context;
    transport.update = empty_update;
    transport.is_connected = empty_connected;
    transport.read = empty_read;
    transport.write = empty_write;
    stream = mn_stream_create(&transport, 0u);
    if (!stream || !mn_stream_send(stream, 3u, "installed-api", 13u) ||
        mn_stream_pending_send_bytes(stream) !=
            MN_STREAM_FRAME_HEADER_BYTES + 13u) {
        fputs("Installed MultiNet framed-stream API could not be used.\n", stderr);
        if (stream) mn_stream_destroy(stream);
        mn_destroy(network);
        return 1;
    }
    mn_stream_destroy(stream);

    printf("Installed MultiNet package works (version %s).\n", mn_version());
    mn_destroy(network);
    return 0;
}
