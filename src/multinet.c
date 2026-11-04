            if (target_name[0] && mn_dns_name_equal(owner, target_name)) {
                memcpy(&ipv4.s_addr, packet + data_offset, 4u);
                if (inet_ntop(AF_INET, &ipv4, ip, sizeof(ip)))
                    mn_copy_text(address_name, sizeof(address_name), owner);
            }