/*
 * SDRNative
 * Device probe example
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "sdrnative.h"

#include <stdio.h>
#include <stdlib.h>

static void print_error(const char *label, sdrn_status st, sdrn_device *dev) {
    sdrn_error_info info;
    sdrn_status info_st = dev
        ? sdrn_get_last_error_info(dev, &info)
        : sdrn_get_last_global_error_info(&info);
    if (info_st == SDRN_OK) {
        fprintf(
            stderr, "%s: %s domain=%s native=%d: %s\n",
            label, sdrn_status_string(st),
            sdrn_error_domain_string((sdrn_error_domain)info.domain),
            (int)info.native_code, info.message
        );
    } else {
        fprintf(stderr, "%s: %s\n", label, sdrn_status_string(st));
    }
}

int main(void) {
    printf("SDRNative %s\n", sdrn_version_string());

    size_t count = 0;
    sdrn_status st = sdrn_list_devices(NULL, 0, &count);
    if (st != SDRN_OK) {
        print_error("Device enumeration failed", st, NULL);
        return 2;
    }
    printf("RTL2832U devices: %zu\n", count);
    if (count == 0) return 1;

    sdrn_device_info *infos = calloc(count, sizeof(*infos));
    if (!infos) return 3;
    st = sdrn_list_devices(infos, count, &count);
    if (st != SDRN_OK) {
        print_error("Enumeration failed", st, NULL);
        free(infos);
        return 2;
    }
    for (size_t i = 0; i < count; ++i) {
        printf(
            "[%zu] %04X:%04X bus=%u addr=%u %s %s serial=%s\n",
            i, infos[i].vid, infos[i].pid,
            infos[i].bus, infos[i].address,
            infos[i].manufacturer[0] ? infos[i].manufacturer : "<manufacturer unavailable>",
            infos[i].product[0] ? infos[i].product : "<product unavailable>",
            infos[i].serial[0] ? infos[i].serial : "<none>"
        );
    }
    free(infos);

    sdrn_device *dev = NULL;
    st = sdrn_open(0, &dev);
    if (st != SDRN_OK) {
        print_error("Open failed", st, NULL);
        return 4;
    }

    sdrn_capabilities caps;
    st = sdrn_get_capabilities(dev, &caps);
    if (st == SDRN_OK) {
        printf("capabilities: flags=0x%08X IQ=%s DVB-T=", caps.flags,
               (caps.flags & SDRN_CAP_IQ) ? "yes" : "no");
        for (unsigned mhz = 6; mhz <= 8; ++mhz) {
            if (caps.dvbt_bandwidth_mask & (1u << mhz)) printf(" %uMHz", mhz);
        }
        printf(" PID-slots=%u xtal=%uHz\n", caps.max_pid_filters, caps.tuner_xtal_hz);
    }

    sdrn_device_profile profile;
    st = sdrn_get_device_profile(dev, &profile);
    if (st == SDRN_OK) {
        printf(
            "profile v%u: demod=%s tuner=%s flags=0x%08X\n",
            profile.profile_version,
            sdrn_demod_family_string((sdrn_demod_family)profile.demod_family),
            sdrn_tuner_family_string((sdrn_tuner_family)profile.tuner_family),
            profile.feature_flags
        );
        printf("RF:");
        for (uint32_t i = 0; i < profile.rf_range_count && i < SDRN_MAX_RF_RANGES; ++i) {
            printf(
                " %.3f..%.3fMHz",
                (double)profile.rf_ranges[i].min_hz / 1e6,
                (double)profile.rf_ranges[i].max_hz / 1e6
            );
        }
        printf("\nIQ rates:");
        for (uint32_t i = 0; i < profile.iq_sample_rate_range_count && i < SDRN_MAX_RATE_RANGES; ++i) {
            printf(
                " %.3f..%.3fMS/s",
                (double)profile.iq_sample_rate_ranges[i].min_hz / 1e6,
                (double)profile.iq_sample_rate_ranges[i].max_hz / 1e6
            );
        }
        printf("\nIQ bandwidths:");
        for (uint32_t i = 0; i < profile.iq_bandwidth_range_count && i < SDRN_MAX_RATE_RANGES; ++i) {
            printf(
                " %.3f..%.3fMHz",
                (double)profile.iq_bandwidth_ranges[i].min_hz / 1e6,
                (double)profile.iq_bandwidth_ranges[i].max_hz / 1e6
            );
        }
        printf("\nDVB-T bandwidths:");
        for (uint32_t i = 0; i < profile.dvbt_bandwidth_count && i < SDRN_MAX_DVBT_BANDWIDTHS; ++i) {
            printf(" %uMHz", profile.dvbt_bandwidth_hz[i] / 1000000u);
        }
        printf("\n");
    }

    sdrn_close(dev);
    return 0;
}