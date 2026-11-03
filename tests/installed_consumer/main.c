#include <stdio.h>
#include "multinet.h"

int main(void) {
    mn_config config;
    mn_instance *network;

    mn_config_init(&config);
    config.enable_broadcast = 0;
    network = mn_create(&config);
    if (!network) {
        fputs("MultiNet consumer could not create an instance.\n", stderr);
        return 1;
    }

    printf("Installed MultiNet package works (version %s).\n", mn_version());
    mn_destroy(network);
    return 0;
}
