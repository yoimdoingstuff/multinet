
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

static mn_socket mn_open_socket(mn_instance *instance, uint16_t port) {
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
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket_handle, (struct sockaddr *)&address,
             (mn_socklen)sizeof(address)) != 0) {
        mn_set_error(instance, "Could not bind the requested UDP port");
        mn_close_socket(socket_handle);
        return MN_BAD_SOCKET;
    }
