static void mn_send_mdns_query(mn_instance *instance) {
    uint8_t packet[MN_MDNS_MAX_PACKET];
    size_t offset = 12u;
    struct sockaddr_in destination;
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
    mn_send_mdns_packet(instance, &destination, packet, offset);
}