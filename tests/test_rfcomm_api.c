/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet_rfcomm.h"

#include <stdio.h>

int main(void) {
    mn_rfcomm *connection;
    mn_rfcomm_info info;
#ifdef _WIN32
    if (!mn_rfcomm_supported()) {
        fprintf(stderr, "Windows RFCOMM backend was not reported as available.\n");
        return 1;
    }
#else
    if (mn_rfcomm_supported()) {
        fprintf(stderr, "Unsupported platform unexpectedly reported RFCOMM support.\n");
        return 1;
    }
#endif
    connection = mn_rfcomm_create();
    if (!connection) {
        fprintf(stderr, "Could not create RFCOMM API instance.\n");
        return 1;
    }
    mn_rfcomm_get_info(connection, &info);
    if (info.state != MN_RFCOMM_IDLE) {
        fprintf(stderr, "New RFCOMM instance did not start idle.\n");
        mn_rfcomm_destroy(connection);
        return 1;
    }
#ifndef _WIN32
    if (info.last_error[0] == '\0') {
        fprintf(stderr, "Unsupported platform did not explain its limitation.\n");
        mn_rfcomm_destroy(connection);
        return 1;
    }
#endif
    mn_rfcomm_close(connection);
    mn_rfcomm_get_info(connection, &info);
    if (info.state != MN_RFCOMM_IDLE) {
        fprintf(stderr, "RFCOMM close did not return to idle state.\n");
        mn_rfcomm_destroy(connection);
        return 1;
    }
    mn_rfcomm_destroy(connection);
    puts("MultiNet RFCOMM API lifecycle test passed.");
    return 0;
}
