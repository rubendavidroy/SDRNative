/*
 * SDRNative
 * Public ABI description
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "sdrnative.h"

#include <string.h>

static uint32_t sdrn_u32_size(size_t value) {
    return (uint32_t)value;
}

static uint32_t sdrn_abi_flags(void) {
    uint32_t flags = 0u;
    const uint16_t one = 1u;
    if (*(const uint8_t *)&one == 1u) {
        flags |= SDRN_ABI_FLAG_LITTLE_ENDIAN;
    }
    if (sizeof(void *) == 8u) {
        flags |= SDRN_ABI_FLAG_POINTER_64;
    }
    if (sizeof(sdrn_status) == 4u &&
        sizeof(sdrn_log_level) == 4u &&
        sizeof(sdrn_mode) == 4u &&
        sizeof(sdrn_state) == 4u &&
        sizeof(sdrn_error_domain) == 4u &&
        sizeof(sdrn_stream_format) == 4u &&
        sizeof(sdrn_tuner_family) == 4u &&
        sizeof(sdrn_demod_family) == 4u) {
        flags |= SDRN_ABI_FLAG_ENUMS_32;
    }
    return flags;
}

uint32_t sdrn_abi_version(void) {
    return SDRN_ABI_VERSION;
}

sdrn_status sdrn_get_abi_info(sdrn_abi_info *out_info) {
    if (!out_info) return SDRN_ERR_INVALID_ARGUMENT;

    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sdrn_u32_size(sizeof(*out_info));
    out_info->info_version = SDRN_ABI_INFO_VERSION;
    out_info->abi_version = SDRN_ABI_VERSION;
    out_info->flags = sdrn_abi_flags();
    out_info->pointer_size = sdrn_u32_size(sizeof(void *));

    out_info->status_size = sdrn_u32_size(sizeof(sdrn_status));
    out_info->log_level_size = sdrn_u32_size(sizeof(sdrn_log_level));
    out_info->mode_size = sdrn_u32_size(sizeof(sdrn_mode));
    out_info->state_size = sdrn_u32_size(sizeof(sdrn_state));
    out_info->error_domain_size = sdrn_u32_size(sizeof(sdrn_error_domain));
    out_info->stream_format_size = sdrn_u32_size(sizeof(sdrn_stream_format));
    out_info->tuner_family_size = sdrn_u32_size(sizeof(sdrn_tuner_family));
    out_info->demod_family_size = sdrn_u32_size(sizeof(sdrn_demod_family));

#define SDRN_ABI_LAYOUT(field, type) \
    out_info->field##_size = sdrn_u32_size(sizeof(type)); \
    out_info->field##_align = sdrn_u32_size(_Alignof(type))

    SDRN_ABI_LAYOUT(device_info, sdrn_device_info);
    SDRN_ABI_LAYOUT(capabilities, sdrn_capabilities);
    SDRN_ABI_LAYOUT(error_info, sdrn_error_info);
    SDRN_ABI_LAYOUT(frequency_range, sdrn_frequency_range);
    SDRN_ABI_LAYOUT(rate_range, sdrn_rate_range);
    SDRN_ABI_LAYOUT(device_profile, sdrn_device_profile);
    SDRN_ABI_LAYOUT(dvbt_tune_info, sdrn_dvbt_tune_info);
    SDRN_ABI_LAYOUT(iq_tune_info, sdrn_iq_tune_info);
    SDRN_ABI_LAYOUT(stream_info, sdrn_stream_info);
    SDRN_ABI_LAYOUT(signal_quality, sdrn_signal_quality);
    SDRN_ABI_LAYOUT(usb_stats, sdrn_usb_stats);

#undef SDRN_ABI_LAYOUT

    return SDRN_OK;
}