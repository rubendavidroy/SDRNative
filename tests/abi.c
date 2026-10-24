/*
 * SDRNative
 * ABI tests
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "sdrnative.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK_LAYOUT(info, field, type) do { \
    assert((info).field##_size == (uint32_t)sizeof(type)); \
    assert((info).field##_align == (uint32_t)_Alignof(type)); \
} while (0)

int main(void) {
    sdrn_abi_info info;

    assert(sdrn_abi_version() == SDRN_ABI_VERSION);
    assert(sdrn_get_abi_info(NULL) == SDRN_ERR_INVALID_ARGUMENT);
    assert(sdrn_get_abi_info(&info) == SDRN_OK);

    assert(info.struct_size == (uint32_t)sizeof(info));
    assert(info.info_version == SDRN_ABI_INFO_VERSION);
    assert(info.abi_version == SDRN_ABI_VERSION);
    assert(info.pointer_size == (uint32_t)sizeof(void *));

    assert(info.status_size == (uint32_t)sizeof(sdrn_status));
    assert(info.log_level_size == (uint32_t)sizeof(sdrn_log_level));
    assert(info.mode_size == (uint32_t)sizeof(sdrn_mode));
    assert(info.state_size == (uint32_t)sizeof(sdrn_state));
    assert(info.error_domain_size == (uint32_t)sizeof(sdrn_error_domain));
    assert(info.stream_format_size == (uint32_t)sizeof(sdrn_stream_format));
    assert(info.tuner_family_size == (uint32_t)sizeof(sdrn_tuner_family));
    assert(info.demod_family_size == (uint32_t)sizeof(sdrn_demod_family));

    CHECK_LAYOUT(info, device_info, sdrn_device_info);
    CHECK_LAYOUT(info, capabilities, sdrn_capabilities);
    CHECK_LAYOUT(info, error_info, sdrn_error_info);
    CHECK_LAYOUT(info, frequency_range, sdrn_frequency_range);
    CHECK_LAYOUT(info, rate_range, sdrn_rate_range);
    CHECK_LAYOUT(info, device_profile, sdrn_device_profile);
    CHECK_LAYOUT(info, dvbt_tune_info, sdrn_dvbt_tune_info);
    CHECK_LAYOUT(info, iq_tune_info, sdrn_iq_tune_info);
    CHECK_LAYOUT(info, stream_info, sdrn_stream_info);
    CHECK_LAYOUT(info, signal_quality, sdrn_signal_quality);
    CHECK_LAYOUT(info, usb_stats, sdrn_usb_stats);

    for (size_t i = 0; i < sizeof(info.reserved) / sizeof(info.reserved[0]); ++i) {
        assert(info.reserved[i] == 0u);
    }

    printf("ABI v%u: pointer=%u bytes, profile=%u/%u, error=%u/%u\n",
           info.abi_version,
           info.pointer_size,
           info.device_profile_size,
           info.device_profile_align,
           info.error_info_size,
           info.error_info_align);
    return 0;
}