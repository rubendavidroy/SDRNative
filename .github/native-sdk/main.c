#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdrnative.h"

int main(void) {
    const char *version = sdrn_version_string();
    if (!version || strcmp(version, "0.1.0") != 0) {
        fprintf(stderr, "unexpected SDRNative version\n");
        return 1;
    }
    if (sdrn_abi_version() != SDRN_ABI_VERSION) {
        fprintf(stderr, "SDRNative ABI mismatch\n");
        return 1;
    }
    size_t count = 0;
    const sdrn_status status = sdrn_list_devices(NULL, 0, &count);
    if (status != SDRN_OK) {
        fprintf(stderr, "device enumeration failed: %s\n", sdrn_status_string(status));
        return 1;
    }
    printf("SDRNative %s ABI %u; devices: %zu\n", version, (unsigned)sdrn_abi_version(), count);
    return 0;
}
