/*
 * SDRNative
 * DVB-T capture example
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "sdrnative.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void usage(const char *program) {
    fprintf(stderr, "usage: %s <frequency_mhz> <6|7|8> <seconds> <output.ts>\n", program);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        usage(argv[0]);
        return 2;
    }

    double frequency_mhz = strtod(argv[1], NULL);
    unsigned bandwidth_mhz = (unsigned)strtoul(argv[2], NULL, 10);
    unsigned seconds = (unsigned)strtoul(argv[3], NULL, 10);
    if (frequency_mhz <= 0.0 || bandwidth_mhz < 6u || bandwidth_mhz > 8u || seconds == 0u) {
        usage(argv[0]);
        return 2;
    }

    FILE *out = fopen(argv[4], "wb");
    if (!out) {
        perror("fopen");
        return 3;
    }

    sdrn_device *device = NULL;
    sdrn_status status = sdrn_open(0, &device);
    if (status != SDRN_OK) {
        fprintf(stderr, "open failed: %s: %s\n", sdrn_status_string(status), sdrn_last_global_error());
        fclose(out);
        return 4;
    }

    sdrn_dvbt_tune_info tune;
    status = sdrn_dvbt_start(device, (uint64_t)(frequency_mhz * 1e6 + 0.5),
                             bandwidth_mhz * 1000000u, &tune);
    if (status != SDRN_OK) {
        fprintf(stderr, "DVB-T start failed: %s: %s\n", sdrn_status_string(status), sdrn_last_error(device));
    } else {
        uint8_t stage = 0u;
        status = sdrn_dvbt_wait_lock(device, 5000u, &stage);
        if (status != SDRN_OK) {
            fprintf(stderr, "DVB-T lock failed (FSM %u): %s\n", (unsigned)stage, sdrn_last_error(device));
        }
    }

    /* Start on a transport-stream packet boundary. */
    enum { PACKET = 188, CHUNK = 256 * 1024, PRIME_SIZE = 2 * 1024 * 1024 };
    unsigned char *prime = NULL;
    unsigned long long total = 0ull;
    if (status == SDRN_OK) {
        prime = malloc(PRIME_SIZE);
        if (!prime) {
            fprintf(stderr, "buffer allocation failed\n");
            status = SDRN_ERR_NO_MEMORY;
        }
    }

    size_t used = 0u;
    size_t sync = 0u;
    int aligned = 0;
    for (unsigned attempt = 0u; status == SDRN_OK && attempt < 8u && !aligned; ++attempt) {
        size_t count = 0u;
        status = sdrn_stream_read(device, prime + used, CHUNK, &count, 800u);
        if (status != SDRN_OK) break;
        used += count;
        for (size_t pos = 0u; pos + PACKET * 4u < used; ++pos) {
            int valid = 1;
            for (size_t k = 0u; k < 5u; ++k) {
                if (prime[pos + k * PACKET] != 0x47u) {
                    valid = 0;
                    break;
                }
            }
            if (valid) {
                sync = pos;
                aligned = 1;
                break;
            }
        }
    }
    if (status == SDRN_OK && !aligned) {
        fprintf(stderr, "no MPEG-TS sync found\n");
        status = SDRN_ERR_IO;
    }
    if (status == SDRN_OK) {
        size_t count = used - sync;
        if (fwrite(prime + sync, 1, count, out) != count) {
            perror("fwrite");
            status = SDRN_ERR_IO;
        } else {
            total += count;
        }
    }
    free(prime);

    unsigned char buffer[CHUNK];
    time_t until = time(NULL) + (time_t)seconds;
    while (status == SDRN_OK && time(NULL) < until) {
        size_t count = 0u;
        status = sdrn_stream_read(device, buffer, sizeof(buffer), &count, 800u);
        if (status != SDRN_OK) {
            fprintf(stderr, "read failed: %s: %s\n", sdrn_status_string(status), sdrn_last_error(device));
            break;
        }
        if (count && fwrite(buffer, 1, count, out) != count) {
            perror("fwrite");
            status = SDRN_ERR_IO;
            break;
        }
        total += count;
    }

    sdrn_close(device);
    if (fclose(out) != 0 && status == SDRN_OK) {
        perror("fclose");
        status = SDRN_ERR_IO;
    }
    if (status != SDRN_OK) return 5;

    fprintf(stderr, "captured %.2f MiB TS -> %s\n", (double)total / (1024.0 * 1024.0), argv[4]);
    return 0;
}
