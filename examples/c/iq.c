/*
 * SDRNative
 * IQ capture example
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "sdrnative.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void log_cb(void *user, sdrn_log_level level, const char *message) {
    (void)user;
    static const char *names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    unsigned i = (unsigned)level;
    if (i > 4) i = 4;
    fprintf(stderr, "[%s] %s\n", names[i], message);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s <frequency_mhz> <sample_rate_msps> <seconds> <output.iq>\n"
        "example: %s 100.7 2.4 4 fm.iq\n",
        argv0, argv0
    );
}

int main(int argc, char **argv) {
    if (argc != 5) {
        usage(argv[0]);
        return 2;
    }

    double freq_mhz = strtod(argv[1], NULL);
    double rate_msps = strtod(argv[2], NULL);
    unsigned seconds = (unsigned)strtoul(argv[3], NULL, 10);
    if (freq_mhz <= 0.0 || rate_msps <= 0.0 || seconds == 0u) {
        usage(argv[0]);
        return 2;
    }

    uint64_t frequency_hz = (uint64_t)(freq_mhz * 1000000.0 + 0.5);
    uint32_t sample_rate_hz = (uint32_t)(rate_msps * 1000000.0 + 0.5);

    FILE *out = fopen(argv[4], "wb");
    if (!out) {
        perror("fopen");
        return 3;
    }

    sdrn_device *dev = NULL;
    sdrn_status st = sdrn_open(0, &dev);
    if (st != SDRN_OK) {
        fprintf(stderr, "open failed: %s: %s\n", sdrn_status_string(st), sdrn_last_global_error());
        fclose(out);
        return 4;
    }
    sdrn_set_log_callback(dev, log_cb, NULL, SDRN_LOG_DEBUG);

    sdrn_iq_tune_info tune;
    st = sdrn_iq_start(dev, frequency_hz, sample_rate_hz, 0u, &tune);
    if (st != SDRN_OK) {
        fprintf(stderr, "IQ start failed: %s: %s\n", sdrn_status_string(st), sdrn_last_error(dev));
        sdrn_close(dev);
        fclose(out);
        return 5;
    }

    fprintf(stderr,
        "IQ ready: RF=%.3f MHz requested=%.6f MS/s actual=%.6f MS/s "
        "BW=%.3f MHz IF=%.3f MHz LO=%.3f MHz\n",
        (double)tune.rf_hz / 1e6,
        (double)tune.requested_sample_rate_hz / 1e6,
        (double)tune.actual_sample_rate_hz / 1e6,
        (double)tune.bandwidth_hz / 1e6,
        (double)tune.if_hz / 1e6,
        (double)tune.lo_hz / 1e6
    );

    sdrn_stream_info info;
    st = sdrn_get_stream_info(dev, &info);
    if (st != SDRN_OK || info.format != SDRN_STREAM_U8_IQ_INTERLEAVED) {
        fprintf(stderr, "unexpected stream format\n");
        sdrn_close(dev);
        fclose(out);
        return 6;
    }

    unsigned char *buffer = malloc(256u * 1024u);
    if (!buffer) {
        sdrn_close(dev);
        fclose(out);
        return 7;
    }

    time_t until = time(NULL) + (time_t)seconds;
    unsigned long long total = 0ull;
    while (time(NULL) < until) {
        size_t got = 0;
        st = sdrn_stream_read(dev, buffer, 256u * 1024u, &got, 800u);
        if (st != SDRN_OK) {
            fprintf(stderr, "stream read failed: %s: %s\n", sdrn_status_string(st), sdrn_last_error(dev));
            break;
        }
        if (got && fwrite(buffer, 1, got, out) != got) {
            perror("fwrite");
            st = SDRN_ERR_IO;
            break;
        }
        total += got;
    }

    free(buffer);
    fflush(out);
    fclose(out);
    sdrn_close(dev);

    fprintf(stderr, "captured %.2f MiB IQ -> %s\n",
            (double)total / (1024.0 * 1024.0), argv[4]);
    return st == SDRN_OK ? 0 : 8;
}