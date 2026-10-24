/*
 * SDRNative
 * Device lifecycle and native modes
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void default_log(void *user, sdrn_log_level level, const char *message) {
    (void)user;
    (void)level;
    (void)message;
}

sdrn_status sdrn_device_create_with_backend(
    const sdrn_usb_ops *ops,
    void *ctx,
    size_t index,
    sdrn_device **out_device
) {
    if (!ops || !out_device || !ops->open || !ops->close ||
        !ops->claim_interface || !ops->release_interface ||
        !ops->endpoint_info || !ops->control_transfer || !ops->bulk_read) {
        sdrn_set_global_error_infof(
            SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE, "invalid USB backend or output pointer"
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    *out_device = NULL;

    sdrn_device *dev = (sdrn_device *)calloc(1, sizeof(*dev));
    if (!dev) {
        sdrn_set_global_error_infof(
            SDRN_ERR_NO_MEMORY, SDRN_ERROR_DOMAIN_INTERNAL, 0,
            SDRN_ERROR_FLAG_NONE, "device state allocation failed"
        );
        if (ops->destroy) ops->destroy(ctx);
        return SDRN_ERR_NO_MEMORY;
    }

    dev->usb = ops;
    dev->usb_ctx = ctx;
    dev->log_fn = default_log;
    dev->log_minimum = SDRN_LOG_INFO;
    dev->mode = SDRN_MODE_NONE;
    dev->usb_recovery_stats.struct_size = (uint32_t)sizeof(dev->usb_recovery_stats);
    dev->usb_recovery_stats.stats_version = SDRN_USB_RECOVERY_STATS_VERSION;
    sdrn_r820t_reset_state(dev);

    sdrn_status st = sdrn_mutex_create(&dev->async_lifecycle_mutex);
    if (st != SDRN_OK) {
        sdrn_set_global_error_infof(
            st, SDRN_ERROR_DOMAIN_INTERNAL, 0, SDRN_ERROR_FLAG_NONE,
            "async mutex allocation failed"
        );
        if (ops->destroy) ops->destroy(ctx);
        free(dev);
        return st;
    }

    char err[256] = {0};
    st = ops->open(
        ctx, index, &dev->usb_handle, &dev->info, err, sizeof(err)
    );
    if (st != SDRN_OK) {
        const int32_t native_code = ops->native_error_code ? ops->native_error_code(ctx) : 0;
        sdrn_set_global_error_infof(
            st, SDRN_ERROR_DOMAIN_USB, native_code, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : "USB open failed"
        );
        sdrn_set_error_infof(
            dev, st, SDRN_ERROR_DOMAIN_USB, native_code, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : "USB open failed"
        );
        if (ops->destroy) ops->destroy(ctx);
        sdrn_mutex_destroy(dev->async_lifecycle_mutex);
        free(dev);
        return st;
    }

    st = ops->claim_interface(
        ctx, dev->usb_handle, SDRN_DEFAULT_INTERFACE, err, sizeof(err)
    );
    if (st != SDRN_OK) {
        const int32_t native_code = ops->native_error_code ? ops->native_error_code(ctx) : 0;
        sdrn_set_global_error_infof(
            st, SDRN_ERROR_DOMAIN_USB, native_code, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : "USB interface 0 claim failed"
        );
        sdrn_set_error_infof(
            dev, st, SDRN_ERROR_DOMAIN_USB, native_code, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : "USB interface 0 claim failed"
        );
        ops->close(ctx, dev->usb_handle);
        if (ops->destroy) ops->destroy(ctx);
        sdrn_mutex_destroy(dev->async_lifecycle_mutex);
        free(dev);
        return st;
    }
    dev->interface_claimed = true;

    uint16_t max_packet = 0;
    bool is_bulk = false;
    st = ops->endpoint_info(
        ctx,
        dev->usb_handle,
        SDRN_DEFAULT_INTERFACE,
        SDRN_TS_ENDPOINT,
        &max_packet,
        &is_bulk,
        err,
        sizeof(err)
    );
    if (st != SDRN_OK || !is_bulk) {
        const sdrn_status endpoint_st = st != SDRN_OK ? st : SDRN_ERR_USB;
        const int32_t native_code = ops->native_error_code ? ops->native_error_code(ctx) : 0;
        sdrn_set_global_error_infof(
            endpoint_st, SDRN_ERROR_DOMAIN_USB, native_code, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : "bulk endpoint 0x81 not found"
        );
        sdrn_set_error_infof(
            dev, endpoint_st, SDRN_ERROR_DOMAIN_USB, native_code,
            SDRN_ERROR_FLAG_NONE, "%s",
            err[0] ? err : "bulk endpoint 0x81 not found"
        );
        ops->release_interface(ctx, dev->usb_handle, SDRN_DEFAULT_INTERFACE);
        ops->close(ctx, dev->usb_handle);
        if (ops->destroy) ops->destroy(ctx);
        sdrn_mutex_destroy(dev->async_lifecycle_mutex);
        free(dev);
        return st != SDRN_OK ? st : SDRN_ERR_USB;
    }

    sdrn_logf(
        dev, SDRN_LOG_INFO,
        "USB %04X:%04X %s opened; EP 0x81 max packet=%u",
        dev->info.vid,
        dev->info.pid,
        dev->info.product[0] ? dev->info.product : "RTL2832U",
        (unsigned)max_packet
    );

    *out_device = dev;
    return SDRN_OK;
}

sdrn_status sdrn_list_devices(
    sdrn_device_info *infos,
    size_t capacity,
    size_t *count
) {
    if (!count) {
        sdrn_set_global_error_infof(
            SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE, "device count output is NULL"
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    *count = 0;

    const sdrn_usb_ops *ops = NULL;
    void *ctx = NULL;
    char err[256] = {0};
    sdrn_clear_global_error();
    sdrn_status st = sdrn_libusb_backend_create(&ops, &ctx, err, sizeof(err));
    if (st != SDRN_OK) {
        sdrn_set_global_error_infof(
            st, SDRN_ERROR_DOMAIN_USB, 0, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : sdrn_status_string(st)
        );
        return st;
    }

    st = ops->list(ctx, infos, capacity, count, err, sizeof(err));
    if (st != SDRN_OK) {
        const int32_t native_code = ops->native_error_code ? ops->native_error_code(ctx) : 0;
        sdrn_set_global_error_infof(
            st, SDRN_ERROR_DOMAIN_USB, native_code, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : sdrn_status_string(st)
        );
    }
    if (ops->destroy) ops->destroy(ctx);
    return st;
}

sdrn_status sdrn_open(size_t index, sdrn_device **out_device) {
    if (!out_device) {
        sdrn_set_global_error_infof(
            SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE, "device output is NULL"
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    *out_device = NULL;

    const sdrn_usb_ops *ops = NULL;
    void *ctx = NULL;
    char err[256] = {0};
    sdrn_clear_global_error();
    sdrn_status st = sdrn_libusb_backend_create(&ops, &ctx, err, sizeof(err));
    if (st != SDRN_OK) {
        sdrn_set_global_error_infof(
            st, SDRN_ERROR_DOMAIN_USB, 0, SDRN_ERROR_FLAG_NONE,
            "%s", err[0] ? err : sdrn_status_string(st)
        );
        return st;
    }
    return sdrn_device_create_with_backend(ops, ctx, index, out_device);
}

void sdrn_close(sdrn_device *device) {
    if (!device) return;

    (void)sdrn_stop(device);

    if (device->usb && device->usb_handle) {
        if (device->interface_claimed && device->usb->release_interface) {
            device->usb->release_interface(
                device->usb_ctx,
                device->usb_handle,
                SDRN_DEFAULT_INTERFACE
            );
        }
        if (device->usb->close) {
            device->usb->close(device->usb_ctx, device->usb_handle);
        }
    }
    if (device->usb && device->usb->destroy) {
        device->usb->destroy(device->usb_ctx);
    }
    sdrn_mutex_destroy(device->async_lifecycle_mutex);
    free(device);
}

sdrn_status sdrn_get_device_info(
    const sdrn_device *device,
    sdrn_device_info *out_info
) {
    if (!device || !out_info) return SDRN_ERR_INVALID_ARGUMENT;
    *out_info = device->info;
    return SDRN_OK;
}

sdrn_status sdrn_get_capabilities(
    const sdrn_device *device,
    sdrn_capabilities *out_caps
) {
    if (!device || !out_caps) return SDRN_ERR_INVALID_ARGUMENT;
    memset(out_caps, 0, sizeof(*out_caps));
    out_caps->flags =
        SDRN_CAP_DVBT |
        SDRN_CAP_HW_PID_FILTER |
        SDRN_CAP_RAW_REGISTERS |
        SDRN_CAP_RAW_I2C |
        SDRN_CAP_RAW_DEMOD |
        SDRN_CAP_R820T_FAMILY |
        SDRN_CAP_IQ;
    out_caps->dvbt_bandwidth_mask = (1u << 6) | (1u << 7) | (1u << 8);
    out_caps->max_pid_filters = SDRN_MAX_PID_FILTER;
    out_caps->tuner_xtal_hz = SDRN_XTAL_HZ;
    out_caps->demod_xtal_hz = SDRN_XTAL_HZ;
    return SDRN_OK;
}

const char *sdrn_tuner_family_string(sdrn_tuner_family family) {
    switch (family) {
        case SDRN_TUNER_R820T_FAMILY: return "r820t-family";
        case SDRN_TUNER_UNKNOWN:
        default: return "unknown";
    }
}

const char *sdrn_demod_family_string(sdrn_demod_family family) {
    switch (family) {
        case SDRN_DEMOD_RTL2832: return "rtl2832";
        case SDRN_DEMOD_UNKNOWN:
        default: return "unknown";
    }
}

sdrn_status sdrn_get_device_profile(
    const sdrn_device *device,
    sdrn_device_profile *out_profile
) {
    if (!device || !out_profile) return SDRN_ERR_INVALID_ARGUMENT;

    memset(out_profile, 0, sizeof(*out_profile));
    out_profile->profile_version = SDRN_DEVICE_PROFILE_VERSION;
    out_profile->feature_flags = SDRN_CAP_GENERIC_MASK;
    out_profile->tuner_family = SDRN_TUNER_R820T_FAMILY;
    out_profile->demod_family = SDRN_DEMOD_RTL2832;
    out_profile->max_pid_filters = SDRN_MAX_PID_FILTER;
    out_profile->tuner_xtal_hz = SDRN_XTAL_HZ;
    out_profile->demod_xtal_hz = SDRN_XTAL_HZ;

    out_profile->rf_range_count = 1u;
    out_profile->rf_ranges[0].min_hz = SDRN_R820T_MIN_FREQUENCY_HZ;
    out_profile->rf_ranges[0].max_hz = SDRN_R820T_MAX_FREQUENCY_HZ;

    /* RTL2832 resampler has an unsupported 300 to 900 kS/s gap */
    out_profile->iq_sample_rate_range_count = 2u;
    out_profile->iq_sample_rate_ranges[0].min_hz = 225001u;
    out_profile->iq_sample_rate_ranges[0].max_hz = 300000u;
    out_profile->iq_sample_rate_ranges[1].min_hz = 900001u;
    out_profile->iq_sample_rate_ranges[1].max_hz = 3200000u;

    out_profile->iq_bandwidth_range_count = 1u;
    out_profile->iq_bandwidth_ranges[0].min_hz = 225000u;
    out_profile->iq_bandwidth_ranges[0].max_hz = 8000000u;

    out_profile->dvbt_bandwidth_count = 3u;
    out_profile->dvbt_bandwidth_hz[0] = 6000000u;
    out_profile->dvbt_bandwidth_hz[1] = 7000000u;
    out_profile->dvbt_bandwidth_hz[2] = 8000000u;

    return SDRN_OK;
}

sdrn_mode sdrn_get_mode(const sdrn_device *device) {
    return device ? device->mode : SDRN_MODE_NONE;
}

sdrn_state sdrn_get_state(const sdrn_device *device) {
    if (!device) return SDRN_STATE_INVALID;
    if (device->mode == SDRN_MODE_DVBT) return SDRN_STATE_DVBT;
    if (device->mode == SDRN_MODE_IQ) return SDRN_STATE_IQ;
    return SDRN_STATE_IDLE;
}

static void remember_first_stop_error(
    sdrn_device *dev, sdrn_status st, sdrn_status *first,
    char *message, size_t message_cap, sdrn_error_info *info
) {
    if (!dev || !first || !message || message_cap == 0u || !info) return;
    if (*first != SDRN_OK || st == SDRN_OK) return;
    *first = st;
    snprintf(message, message_cap, "%s", dev->last_error);
    *info = dev->last_error_info;
}

static sdrn_status stop_dvbt(sdrn_device *dev) {
    sdrn_status first = SDRN_OK;
    sdrn_status st;
    char first_error[sizeof(dev->last_error)] = {0};
    sdrn_error_info first_info;
    memset(&first_info, 0, sizeof(first_info));

    if (dev->frontend_initialized) {
        st = sdrn_rtl2832_disable_pid_filter(dev);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }

    /* stand tuner down while RTL2832 repeater is still usable */
    /* final gate close must happen before frontend soft reset */
    if (dev->tuner_initialized) {
        if (!dev->i2c_gate_open) {
            st = sdrn_i2c_gate_set(dev, true);
            remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
        }
        if (dev->i2c_gate_open) {
            st = sdrn_r820t_standby(dev);
            remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
            st = sdrn_i2c_gate_set(dev, false);
            remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
        }
    } else if (dev->i2c_gate_open) {
        st = sdrn_i2c_gate_set(dev, false);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }

    if (dev->frontend_initialized) {
        st = sdrn_rtl2832_release(dev);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }

    if (dev->ts_configured) {
        st = sdrn_ts_restore(dev);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }

    if (dev->power_configured) {
        st = sdrn_power_restore(dev);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }

    dev->frontend_initialized = false;
    dev->tuner_initialized = false;
    dev->ts_configured = false;
    dev->power_configured = false;
    dev->stream_consumed = false;
    dev->stream_transport_armed = false;
    dev->dvbt_post_lock_flush_done = false;
    dev->i2c_gate_open = false;
    dev->mode = SDRN_MODE_NONE;
    memset(&dev->tune, 0, sizeof(dev->tune));
    sdrn_r820t_reset_state(dev);

    if (first != SDRN_OK && first_error[0] != '\0') {
        snprintf(dev->last_error, sizeof(dev->last_error), "%s", first_error);
        dev->last_error_info = first_info;
    }
    return first;
}


static sdrn_status stop_iq(sdrn_device *dev) {
    sdrn_status first = SDRN_OK;
    sdrn_status st;
    char first_error[sizeof(dev->last_error)] = {0};
    sdrn_error_info first_info;
    memset(&first_info, 0, sizeof(first_info));

    if (dev->tuner_initialized) {
        if (!dev->i2c_gate_open) {
            st = sdrn_i2c_gate_set(dev, true);
            remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
        }
        if (dev->i2c_gate_open) {
            st = sdrn_r820t_standby(dev);
            remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
            st = sdrn_i2c_gate_set(dev, false);
            remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
        }
    } else if (dev->i2c_gate_open) {
        st = sdrn_i2c_gate_set(dev, false);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }

    /* iq mode changes more demod state than generic power snapshot covers */
    /* restore pre iq image while demod is still powered */
    st = sdrn_rtl2832_iq_restore_state(dev);
    remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);

    if (dev->ts_configured) {
        st = sdrn_ts_restore(dev);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }
    if (dev->power_configured) {
        st = sdrn_power_restore(dev);
        remember_first_stop_error(dev, st, &first, first_error, sizeof(first_error), &first_info);
    }

    dev->frontend_initialized = false;
    dev->tuner_initialized = false;
    dev->ts_configured = false;
    dev->power_configured = false;
    dev->stream_consumed = false;
    dev->stream_transport_armed = false;
    dev->dvbt_post_lock_flush_done = false;
    dev->iq_configured = false;
    dev->i2c_gate_open = false;
    dev->mode = SDRN_MODE_NONE;
    memset(&dev->iq_tune, 0, sizeof(dev->iq_tune));
    sdrn_r820t_reset_state(dev);
    if (first != SDRN_OK && first_error[0] != '\0') {
        snprintf(dev->last_error, sizeof(dev->last_error), "%s", first_error);
        dev->last_error_info = first_info;
    }
    return first;
}

static void abandon_native_state_after_disconnect(sdrn_device *dev) {
    if (!dev) return;

    dev->frontend_initialized = false;
    dev->tuner_initialized = false;
    dev->ts_configured = false;
    dev->power_configured = false;
    dev->stream_consumed = false;
    dev->stream_transport_armed = false;
    dev->dvbt_post_lock_flush_done = false;
    dev->iq_configured = false;
    dev->i2c_gate_open = false;
    dev->mode = SDRN_MODE_NONE;
    dev->power_snapshot.valid = false;
    dev->ts_snapshot.valid = false;
    dev->iq_demod_snapshot.valid = false;
    memset(&dev->tune, 0, sizeof(dev->tune));
    memset(&dev->iq_tune, 0, sizeof(dev->iq_tune));
    sdrn_r820t_reset_state(dev);
}

static void capture_current_error(
    const sdrn_device *device,
    sdrn_status *status,
    char *message,
    size_t message_cap,
    sdrn_error_info *info
) {
    if (!device || !status || !message || message_cap == 0u || !info) return;
    *status = (sdrn_status)device->last_error_info.status;
    snprintf(message, message_cap, "%s", device->last_error);
    *info = device->last_error_info;
}

static sdrn_status finalize_stop_after_disconnect(
    sdrn_device *device,
    sdrn_status first,
    char *first_error,
    size_t first_error_cap,
    sdrn_error_info *first_info,
    int32_t confirmed_native
) {
    if (!device || !first_error || first_error_cap == 0u || !first_info) {
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    /* WinUSB removal can surface as STALL or PIPE before NO_DEVICE */
    /* confirmed disappearance replaces provisional transport fault */
    if (confirmed_native == 0 &&
        device->last_error_info.status == (int32_t)SDRN_ERR_NOT_FOUND) {
        confirmed_native = device->last_error_info.native_code;
    }
    const bool terminal_context =
        first != SDRN_OK &&
        (first_info->flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0u;

    if (first != SDRN_ERR_NOT_FOUND) {
        char prior[160] = {0};
        if (first_error[0] != '\0') {
            snprintf(prior, sizeof(prior), "%.159s", first_error);
        }
        const uint32_t flags = SDRN_ERROR_FLAG_DEVICE_GONE |
            (terminal_context ? SDRN_ERROR_FLAG_TERMINAL_STREAM : 0u);
        if (terminal_context && prior[0] != '\0') {
            sdrn_set_error_infof(
                device, SDRN_ERR_NOT_FOUND, SDRN_ERROR_DOMAIN_STREAM,
                confirmed_native, flags,
                "device disconnected after stream fault: %s",
                prior
            );
        } else if (prior[0] != '\0') {
            sdrn_set_error_infof(
                device, SDRN_ERR_NOT_FOUND, SDRN_ERROR_DOMAIN_USB,
                confirmed_native, flags,
                "device disconnected during stop: %s",
                prior
            );
        } else {
            sdrn_set_error_infof(
                device, SDRN_ERR_NOT_FOUND, SDRN_ERROR_DOMAIN_USB,
                confirmed_native, flags,
                "device disconnected during stop; reopen after re-enumeration"
            );
        }
        capture_current_error(
            device, &first, first_error, first_error_cap, first_info
        );
    } else {
        first_info->flags |= SDRN_ERROR_FLAG_DEVICE_GONE;
        if (terminal_context) first_info->flags |= SDRN_ERROR_FLAG_TERMINAL_STREAM;
    }

    abandon_native_state_after_disconnect(device);
    snprintf(device->last_error, sizeof(device->last_error), "%s", first_error);
    device->last_error_info = *first_info;
    sdrn_logf(
        device, SDRN_LOG_WARN,
        "device disconnected; skipping hardware restore"
    );
    return first == SDRN_OK ? SDRN_ERR_NOT_FOUND : first;
}

sdrn_status sdrn_stop(sdrn_device *device) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;

    /* keep terminal stream fault as primary failure through cleanup */
    sdrn_status first = SDRN_OK;
    char first_error[sizeof(device->last_error)] = {0};
    sdrn_error_info first_info;
    memset(&first_info, 0, sizeof(first_info));

    const bool async_present = device->async_state != NULL;
    const bool has_native_ownership =
        async_present ||
        device->mode != SDRN_MODE_NONE ||
        device->power_configured ||
        device->ts_configured ||
        device->tuner_initialized ||
        device->frontend_initialized ||
        device->iq_configured ||
        device->device_gone;
    /* public reader may still be publishing terminal state */
    /* inspect shared error record only after reader leaves */
    if (!async_present && has_native_ownership &&
        (device->last_error_info.flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0u &&
        device->last_error_info.status != (int32_t)SDRN_OK) {
        capture_current_error(device, &first, first_error, sizeof(first_error), &first_info);
    }

    sdrn_status async_terminal = SDRN_OK;
    int32_t async_native = 0;
    char async_error[256] = {0};
    const bool have_async_terminal = sdrn_async_terminal_snapshot(
        device, &async_terminal, &async_native, async_error, sizeof(async_error)
    );
    int32_t confirmed_disconnect_native = 0;
    if (have_async_terminal && async_terminal == SDRN_ERR_NOT_FOUND) {
        device->device_gone = true;
        confirmed_disconnect_native = async_native;
    }

    if (device->async_state) {
        sdrn_status async_st = sdrn_async_stop_internal(device, false);
        if (async_st == SDRN_ERR_NOT_FOUND) {
            device->device_gone = true;
            confirmed_disconnect_native = device->last_error_info.native_code;
        }
        if (!have_async_terminal && async_st != SDRN_OK) {
            capture_current_error(
                device, &first, first_error, sizeof(first_error), &first_info
            );
        }
    }

    /* producer terminal error stays primary over stop cleanup failure */
    /* publish after blocked consumer leaves shared error state */
    if (have_async_terminal) {
        uint32_t flags = SDRN_ERROR_FLAG_TERMINAL_STREAM;
        if (async_terminal == SDRN_ERR_NOT_FOUND) flags |= SDRN_ERROR_FLAG_DEVICE_GONE;
        sdrn_set_error_infof(
            device, async_terminal, SDRN_ERROR_DOMAIN_STREAM, async_native, flags,
            "async stream failed: %s",
            async_error[0] ? async_error : sdrn_status_string(async_terminal)
        );
        capture_current_error(
            device, &first, first_error, sizeof(first_error), &first_info
        );
    }

    /* usb device is already gone */
    /* do not attempt hardware restore writes here */
    if (device->device_gone) {
        return finalize_stop_after_disconnect(
            device, first, first_error, sizeof(first_error), &first_info,
            confirmed_disconnect_native
        );
    }

    if (device->mode == SDRN_MODE_NONE &&
        !device->power_configured &&
        !device->ts_configured &&
        !device->tuner_initialized &&
        !device->frontend_initialized &&
        !device->iq_configured) {
        if (first_error[0] != '\0') {
            snprintf(device->last_error, sizeof(device->last_error), "%s", first_error);
            device->last_error_info = first_info;
        } else {
            sdrn_clear_error(device);
        }
        return first;
    }

    if (device->stream_transport_armed) {
        sdrn_status quiesce_st = sdrn_stream_quiesce_endpoint(device);
        if (quiesce_st == SDRN_ERR_NOT_FOUND) {
            confirmed_disconnect_native = device->last_error_info.native_code;
        }
        if (quiesce_st != SDRN_OK && first == SDRN_OK) {
            first = quiesce_st;
            snprintf(first_error, sizeof(first_error), "%s", device->last_error);
            first_info = device->last_error_info;
        }
        if (device->device_gone || quiesce_st == SDRN_ERR_NOT_FOUND) {
            return finalize_stop_after_disconnect(
                device, first, first_error, sizeof(first_error), &first_info,
                confirmed_disconnect_native
            );
        }
    }

    sdrn_status st;
    if (device->mode == SDRN_MODE_IQ || device->iq_configured) {
        st = stop_iq(device);
    } else if (device->mode == SDRN_MODE_DVBT || device->power_configured) {
        st = stop_dvbt(device);
    } else {
        sdrn_set_error_infof(
            device, SDRN_ERR_UNSUPPORTED, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "unsupported mode %d", (int)device->mode
        );
        st = SDRN_ERR_UNSUPPORTED;
    }

    if (first == SDRN_OK && st != SDRN_OK) {
        first = st;
        snprintf(first_error, sizeof(first_error), "%s", device->last_error);
        first_info = device->last_error_info;
    }

    if (first_error[0] != '\0') {
        snprintf(device->last_error, sizeof(device->last_error), "%s", first_error);
        device->last_error_info = first_info;
    }

    if (first == SDRN_OK) {
        sdrn_clear_error(device);
        sdrn_logf(device, SDRN_LOG_INFO, "hardware state restored");
    }
    return first;
}

sdrn_status sdrn_get_usb_stats(
    const sdrn_device *device,
    sdrn_usb_stats *out_stats
) {
    if (!device || !out_stats) return SDRN_ERR_INVALID_ARGUMENT;
    *out_stats = device->usb_stats;
    return SDRN_OK;
}

sdrn_status sdrn_get_usb_recovery_stats(
    const sdrn_device *device,
    sdrn_usb_recovery_stats *out_stats
) {
    if (!device || !out_stats) return SDRN_ERR_INVALID_ARGUMENT;
    *out_stats = device->usb_recovery_stats;
    out_stats->struct_size = (uint32_t)sizeof(*out_stats);
    out_stats->stats_version = SDRN_USB_RECOVERY_STATS_VERSION;
    return SDRN_OK;
}

sdrn_status sdrn_reset_usb_stats(sdrn_device *device) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    memset(&device->usb_stats, 0, sizeof(device->usb_stats));
    memset(&device->usb_recovery_stats, 0, sizeof(device->usb_recovery_stats));
    device->usb_recovery_stats.struct_size = (uint32_t)sizeof(device->usb_recovery_stats);
    device->usb_recovery_stats.stats_version = SDRN_USB_RECOVERY_STATS_VERSION;
    return SDRN_OK;
}

sdrn_status sdrn_get_stream_info(
    const sdrn_device *device,
    sdrn_stream_info *out_info
) {
    if (!device || !out_info) return SDRN_ERR_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));

    if (device->mode == SDRN_MODE_DVBT) {
        out_info->format = SDRN_STREAM_MPEG_TS;
        out_info->packet_size_bytes = 188u;
        return SDRN_OK;
    }
    if (device->mode == SDRN_MODE_IQ) {
        out_info->format = SDRN_STREAM_U8_IQ_INTERLEAVED;
        out_info->sample_rate_hz = device->iq_tune.actual_sample_rate_hz;
        out_info->bytes_per_complex_sample = 2u;
        return SDRN_OK;
    }

    out_info->format = SDRN_STREAM_NONE;
    return SDRN_OK;
}

static bool valid_r820t_frequency(uint64_t frequency_hz) {
    return frequency_hz >= SDRN_R820T_MIN_FREQUENCY_HZ &&
           frequency_hz <= SDRN_R820T_MAX_FREQUENCY_HZ;
}

sdrn_status sdrn_iq_start(
    sdrn_device *device,
    uint64_t frequency_hz,
    uint32_t sample_rate_hz,
    uint32_t bandwidth_hz,
    sdrn_iq_tune_info *out_tune
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (!valid_r820t_frequency(frequency_hz)) {
        sdrn_set_error_infof(device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0, SDRN_ERROR_FLAG_NONE, "frequency outside R820T range (24..1766 MHz)");
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    if (device->mode != SDRN_MODE_NONE || device->power_configured) {
        sdrn_set_error_infof(device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0, SDRN_ERROR_FLAG_NONE, "device already in use");
        return SDRN_ERR_STATE;
    }
    if (bandwidth_hz == 0u) bandwidth_hz = sample_rate_hz;

    sdrn_status st;
    st = sdrn_iq_usb_power_on(device);
    if (st != SDRN_OK) return st;
    device->power_configured = true;
    device->ts_configured = true; /* endpoint snapshot is active */
    device->iq_configured = true;

    st = sdrn_rtl2832_iq_snapshot_state(device);
    if (st != SDRN_OK) goto fail;

    st = sdrn_rtl2832_iq_initialize(device);
    if (st != SDRN_OK) goto fail;

    uint32_t actual_rate = 0u;
    st = sdrn_rtl2832_iq_set_sample_rate(device, sample_rate_hz, &actual_rate);
    if (st != SDRN_OK) goto fail;

    st = sdrn_i2c_gate_set(device, true);
    if (st != SDRN_OK) goto fail;

    uint8_t tuner_id = 0;
    st = sdrn_r820t_probe(device, &tuner_id);
    if (st != SDRN_OK) goto fail;
    st = sdrn_r820t_initialize_fast(device);
    if (st != SDRN_OK) goto fail;
    device->tuner_initialized = true;

    uint32_t if_hz = 0u;
    uint64_t lo_hz = 0ull;
    st = sdrn_r820t_tune_iq(device, frequency_hz, bandwidth_hz, &if_hz, &lo_hz);
    if (st != SDRN_OK) goto fail;

    st = sdrn_i2c_gate_set(device, false);
    if (st != SDRN_OK) goto fail;

    st = sdrn_rtl2832_iq_set_if(device, if_hz);
    if (st != SDRN_OK) goto fail;

    /* iq bringup leaves endpoint A reset at EPA_CTL 0x1002 */
    /* reset and release FIFO before first stream read */
    st = sdrn_stream_reset_endpoint(device);
    if (st != SDRN_OK) goto fail;

    memset(&device->iq_tune, 0, sizeof(device->iq_tune));
    device->iq_tune.rf_hz = frequency_hz;
    device->iq_tune.requested_sample_rate_hz = sample_rate_hz;
    device->iq_tune.actual_sample_rate_hz = actual_rate;
    device->iq_tune.bandwidth_hz = bandwidth_hz;
    device->iq_tune.if_hz = if_hz;
    device->iq_tune.lo_hz = lo_hz;
    device->iq_tune.tuner_pll_locked = 1u;
    device->mode = SDRN_MODE_IQ;
    if (out_tune) *out_tune = device->iq_tune;

    sdrn_logf(
        device, SDRN_LOG_INFO,
        "IQ started: %.3f MHz, %.3f MS/s, BW %.3f MHz",
        (double)frequency_hz / 1e6,
        (double)actual_rate / 1e6,
        (double)bandwidth_hz / 1e6
    );
    return SDRN_OK;

fail:
    {
        char primary_error[sizeof(device->last_error)];
        const sdrn_error_info primary_info = device->last_error_info;
        snprintf(primary_error, sizeof(primary_error), "%s", device->last_error);
        (void)stop_iq(device);
        if (primary_error[0] != '\0') {
            snprintf(device->last_error, sizeof(device->last_error), "%s", primary_error);
            device->last_error_info = primary_info;
        }
    }
    return st;
}

sdrn_status sdrn_iq_tune(
    sdrn_device *device,
    uint64_t frequency_hz,
    sdrn_iq_tune_info *out_tune
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_IQ) {
        sdrn_set_error_infof(device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0, SDRN_ERROR_FLAG_NONE, "IQ mode not active");
        return SDRN_ERR_STATE;
    }
    if (!valid_r820t_frequency(frequency_hz)) {
        sdrn_set_error_infof(device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0, SDRN_ERROR_FLAG_NONE, "frequency outside R820T range (24..1766 MHz)");
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    sdrn_status st = sdrn_i2c_gate_set(device, true);
    if (st != SDRN_OK) return st;
    uint32_t if_hz = 0u;
    uint64_t lo_hz = 0ull;
    st = sdrn_r820t_tune_iq(
        device, frequency_hz, device->iq_tune.bandwidth_hz, &if_hz, &lo_hz
    );
    sdrn_status close_st = sdrn_i2c_gate_set(device, false);
    if (st != SDRN_OK) return st;
    if (close_st != SDRN_OK) return close_st;

    st = sdrn_rtl2832_iq_set_if(device, if_hz);
    if (st != SDRN_OK) return st;

    device->iq_tune.rf_hz = frequency_hz;
    device->iq_tune.if_hz = if_hz;
    device->iq_tune.lo_hz = lo_hz;
    device->iq_tune.tuner_pll_locked = 1u;
    if (out_tune) *out_tune = device->iq_tune;
    return SDRN_OK;
}

sdrn_status sdrn_iq_set_sample_rate(
    sdrn_device *device,
    uint32_t sample_rate_hz,
    uint32_t *out_actual_sample_rate_hz
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_IQ) {
        sdrn_set_error_infof(device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0, SDRN_ERROR_FLAG_NONE, "IQ mode not active");
        return SDRN_ERR_STATE;
    }

    /* default tuner bandwidth follows sample rate */
    const bool auto_bandwidth =
        device->iq_tune.bandwidth_hz == device->iq_tune.requested_sample_rate_hz;

    uint32_t actual = 0u;
    sdrn_status st = sdrn_rtl2832_iq_set_sample_rate(device, sample_rate_hz, &actual);
    if (st != SDRN_OK) return st;

    if (auto_bandwidth) {
        st = sdrn_iq_set_bandwidth(device, sample_rate_hz, NULL);
        if (st != SDRN_OK) return st;
    } else {
        /* sample rate reset clears IF state */
        st = sdrn_rtl2832_iq_set_if(device, device->iq_tune.if_hz);
        if (st != SDRN_OK) return st;
    }

    device->iq_tune.requested_sample_rate_hz = sample_rate_hz;
    device->iq_tune.actual_sample_rate_hz = actual;
    if (out_actual_sample_rate_hz) *out_actual_sample_rate_hz = actual;
    return SDRN_OK;
}

sdrn_status sdrn_iq_set_bandwidth(
    sdrn_device *device,
    uint32_t bandwidth_hz,
    sdrn_iq_tune_info *out_tune
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_IQ) {
        sdrn_set_error_infof(device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0, SDRN_ERROR_FLAG_NONE, "IQ mode not active");
        return SDRN_ERR_STATE;
    }

    sdrn_status st = sdrn_i2c_gate_set(device, true);
    if (st != SDRN_OK) return st;

    uint32_t if_hz = 0u;
    uint64_t lo_hz = 0ull;
    st = sdrn_r820t_tune_iq(
        device, device->iq_tune.rf_hz, bandwidth_hz, &if_hz, &lo_hz
    );
    sdrn_status close_st = sdrn_i2c_gate_set(device, false);
    if (st != SDRN_OK) return st;
    if (close_st != SDRN_OK) return close_st;

    st = sdrn_rtl2832_iq_set_if(device, if_hz);
    if (st != SDRN_OK) return st;

    device->iq_tune.bandwidth_hz = bandwidth_hz;
    device->iq_tune.if_hz = if_hz;
    device->iq_tune.lo_hz = lo_hz;
    device->iq_tune.tuner_pll_locked = 1u;
    if (out_tune) *out_tune = device->iq_tune;
    return SDRN_OK;
}

static bool valid_dvbt_bandwidth(uint32_t bw) {
    return bw == 6000000u || bw == 7000000u || bw == 8000000u;
}

sdrn_status sdrn_dvbt_start(
    sdrn_device *device,
    uint64_t frequency_hz,
    uint32_t bandwidth_hz,
    sdrn_dvbt_tune_info *out_tune
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (!valid_dvbt_bandwidth(bandwidth_hz)) {
        sdrn_set_error_infof(device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0, SDRN_ERROR_FLAG_NONE, "DVB-T bandwidth must be 6, 7 or 8 MHz");
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    if (!valid_r820t_frequency(frequency_hz)) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "frequency outside R820T range (%.1f..%.1f MHz)",
            (double)SDRN_R820T_MIN_FREQUENCY_HZ / 1e6,
            (double)SDRN_R820T_MAX_FREQUENCY_HZ / 1e6
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    if (device->mode != SDRN_MODE_NONE || device->power_configured) {
        sdrn_set_error_infof(device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0, SDRN_ERROR_FLAG_NONE, "device already in use");
        return SDRN_ERR_STATE;
    }

    sdrn_status st;
    device->dvbt_post_lock_flush_done = false;

    st = sdrn_power_on_dvb(device);
    if (st != SDRN_OK) goto fail;
    device->power_configured = true;

    st = sdrn_rtl2832_attach(device);
    if (st != SDRN_OK) goto fail;
    /* failed bringup from here leaves demod reset */
    device->frontend_initialized = true;
    st = sdrn_rtl2832_initialize_r820t(device);
    if (st != SDRN_OK) goto fail;

    st = sdrn_rtl2832_disable_pid_filter(device);
    if (st != SDRN_OK) goto fail;

    st = sdrn_ts_configure(device);
    if (st != SDRN_OK) goto fail;
    device->ts_configured = true;

    st = sdrn_i2c_gate_set(device, true);
    if (st != SDRN_OK) goto fail;

    uint8_t tuner_id = 0;
    st = sdrn_r820t_probe(device, &tuner_id);
    if (st != SDRN_OK) goto fail;

    st = sdrn_r820t_prepare_dvbt(device);
    if (st != SDRN_OK) goto fail;
    device->tuner_initialized = true;

    /* dvb setup goes from base table straight into TV standard config */
    /* iq keeps its separate fast init path */

    st = sdrn_r820t_tune_dvbt(device, frequency_hz, bandwidth_hz, &device->tune);
    if (st != SDRN_OK) goto fail;

    st = sdrn_i2c_gate_set(device, false);
    if (st != SDRN_OK) goto fail;

    st = sdrn_rtl2832_configure_bandwidth(
        device, bandwidth_hz, device->tune.if_hz
    );
    if (st != SDRN_OK) goto fail;

    /* mode change needs a fresh stream boundary */
    /* stale iq bytes can already be queued in endpoint A FIFO */
    /* release endpoint 0x81 only after dvb setup is complete */
    st = sdrn_stream_reset_endpoint(device);
    if (st != SDRN_OK) goto fail;

    device->mode = SDRN_MODE_DVBT;
    if (out_tune) *out_tune = device->tune;
    sdrn_logf(
        device, SDRN_LOG_INFO,
        "DVB-T started: %.3f MHz, BW %u MHz, IF %.3f MHz",
        (double)frequency_hz / 1e6,
        (unsigned)(bandwidth_hz / 1000000u),
        (double)device->tune.if_hz / 1e6
    );
    return SDRN_OK;

fail:
    {
        char primary_error[sizeof(device->last_error)];
        const sdrn_error_info primary_info = device->last_error_info;
        snprintf(primary_error, sizeof(primary_error), "%s", device->last_error);
        (void)stop_dvbt(device);
        if (primary_error[0] != '\0') {
            snprintf(device->last_error, sizeof(device->last_error), "%s", primary_error);
            device->last_error_info = primary_info;
        }
    }
    return st;
}

sdrn_status sdrn_dvbt_tune(
    sdrn_device *device,
    uint64_t frequency_hz,
    uint32_t bandwidth_hz,
    sdrn_dvbt_tune_info *out_tune
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_DVBT) {
        sdrn_set_error_infof(device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0, SDRN_ERROR_FLAG_NONE, "DVB-T mode not active");
        return SDRN_ERR_STATE;
    }
    if (!valid_dvbt_bandwidth(bandwidth_hz)) {
        sdrn_set_error_infof(device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0, SDRN_ERROR_FLAG_NONE, "DVB-T bandwidth must be 6, 7 or 8 MHz");
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    if (!valid_r820t_frequency(frequency_hz)) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "frequency outside R820T range (%.1f..%.1f MHz)",
            (double)SDRN_R820T_MIN_FREQUENCY_HZ / 1e6,
            (double)SDRN_R820T_MAX_FREQUENCY_HZ / 1e6
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    /* retune after endpoint traffic uses clean stop start lifecycle */
    /* before first stream an in place retune is safe */
    if (device->stream_transport_armed) {
        sdrn_logf(
            device, SDRN_LOG_DEBUG,
            "reinitializing DVB-T transport for retune"
        );
        sdrn_status restart_st = sdrn_stop(device);
        if (restart_st != SDRN_OK) return restart_st;
        return sdrn_dvbt_start(device, frequency_hz, bandwidth_hz, out_tune);
    }

    sdrn_status st = sdrn_i2c_gate_set(device, true);
    if (st != SDRN_OK) return st;

    st = sdrn_r820t_tune_dvbt(device, frequency_hz, bandwidth_hz, &device->tune);
    sdrn_status close_st = sdrn_i2c_gate_set(device, false);
    if (st != SDRN_OK) return st;
    if (close_st != SDRN_OK) return close_st;

    st = sdrn_rtl2832_configure_bandwidth(
        device, bandwidth_hz, device->tune.if_hz
    );
    if (st != SDRN_OK) return st;

    /* new channel can queue bytes before lock */
    /* next lock establishes a fresh TS boundary */
    device->dvbt_post_lock_flush_done = false;
    device->stream_consumed = false;

    if (out_tune) *out_tune = device->tune;
    return SDRN_OK;
}

sdrn_status sdrn_dvbt_wait_lock(
    sdrn_device *device,
    uint32_t timeout_ms,
    uint8_t *out_fsm_stage
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_DVBT) {
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "DVB-T mode not active"
        );
        return SDRN_ERR_STATE;
    }

    uint64_t deadline = sdrn_now_ms() + (uint64_t)timeout_ms;
    uint8_t stage = 0;
    uint8_t highest = 0;
    uint8_t last_logged = 0xFFu;
    do {
        sdrn_status st = sdrn_rtl2832_get_fsm(device, &stage);
        if (st != SDRN_OK) return st;
        if (stage > highest) highest = stage;
        if (stage != last_logged) {
            if (stage == 11u) {
                sdrn_logf(device, SDRN_LOG_DEBUG, "DVB-T locked (FSM 11)");
            } else if (stage == 10u) {
                sdrn_logf(device, SDRN_LOG_DEBUG, "DVB-T FSM 10; waiting for TS sync");
            } else {
                sdrn_logf(device, SDRN_LOG_DEBUG, "DVB-T acquiring (FSM %u)", stage);
            }
            last_logged = stage;
        }
        if (stage == 11u) {
            if (!device->dvbt_post_lock_flush_done) {
                sdrn_status flush_st = sdrn_stream_reset_endpoint(device);
                if (flush_st != SDRN_OK) return flush_st;
                device->dvbt_post_lock_flush_done = true;
                device->stream_consumed = false;
                sdrn_logf(
                    device, SDRN_LOG_DEBUG,
                    "TS FIFO flushed after lock (FSM 11)"
                );
            }
            if (out_fsm_stage) *out_fsm_stage = stage;
            return SDRN_OK;
        }
        sdrn_sleep_ms(50u);
    } while (sdrn_now_ms() < deadline);

    if (out_fsm_stage) *out_fsm_stage = stage;
    sdrn_set_error_infof(
        device, SDRN_ERR_NOT_LOCKED, SDRN_ERROR_DOMAIN_DEMOD, 0,
        SDRN_ERROR_FLAG_NONE,
        "DVB-T lock timed out (highest FSM %u, current %u)",
        highest,
        stage
    );
    return SDRN_ERR_NOT_LOCKED;
}

sdrn_status sdrn_dvbt_get_signal_quality(
    sdrn_device *device,
    sdrn_signal_quality *out_quality
) {
    if (!device || !out_quality) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_DVBT) {
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "DVB-T mode not active"
        );
        return SDRN_ERR_STATE;
    }
    return sdrn_rtl2832_get_quality(device, out_quality);
}

sdrn_status sdrn_stream_read(
    sdrn_device *device,
    void *buffer,
    size_t capacity,
    size_t *out_read,
    uint32_t timeout_ms
) {
    if (!device || !buffer || capacity == 0 || !out_read) {
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    *out_read = 0;
    if (device->device_gone) {
        sdrn_set_error_infof(
            device, SDRN_ERR_NOT_FOUND, SDRN_ERROR_DOMAIN_STREAM, 0,
            SDRN_ERROR_FLAG_DEVICE_GONE,
            "device disconnected; reopen the handle"
        );
        return SDRN_ERR_NOT_FOUND;
    }
    if (device->async_state) {
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE,
            "stop async streaming before synchronous reads"
        );
        return SDRN_ERR_STATE;
    }
    if ((device->mode != SDRN_MODE_DVBT && device->mode != SDRN_MODE_IQ) ||
        !device->ts_configured) {
        sdrn_set_error_infof(device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0, SDRN_ERROR_FLAG_NONE, "no streaming mode active");
        return SDRN_ERR_STATE;
    }
    if (capacity > 0x7fffffffU) {
        sdrn_set_error_infof(device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0, SDRN_ERROR_FLAG_NONE, "read buffer exceeds maximum size");
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    int transferred = 0;
    char err[256] = {0};
    device->usb_stats.bulk_reads++;
    /* any armed bulk request creates transport lifetime */
    /* timeout or zero bytes still require endpoint quiescence */
    device->stream_transport_armed = true;
    sdrn_status st = device->usb->bulk_read(
        device->usb_ctx,
        device->usb_handle,
        SDRN_TS_ENDPOINT,
        (uint8_t *)buffer,
        (int)capacity,
        timeout_ms,
        &transferred,
        err,
        sizeof(err)
    );
    if (st == SDRN_ERR_TIMEOUT) {
        device->usb_stats.bulk_timeouts++;
        /* TIMEOUT can still carry useful partial data */
        if (transferred > 0) {
            *out_read = (size_t)transferred;
            device->usb_stats.bulk_bytes += (uint64_t)transferred;
            device->stream_consumed = true;
        }
        return SDRN_OK;
    }
    if (st != SDRN_OK) {
        device->usb_stats.bulk_errors++;
        uint32_t flags = SDRN_ERROR_FLAG_NONE;
        if (st == SDRN_ERR_NOT_FOUND) {
            device->device_gone = true;
            flags |= SDRN_ERROR_FLAG_DEVICE_GONE;
        }
        sdrn_set_error_infof(
            device, st, SDRN_ERROR_DOMAIN_STREAM, sdrn_usb_native_error(device),
            flags, "bulk read failed: %s",
            err[0] ? err : sdrn_status_string(st)
        );
        return st;
    }
    if (transferred < 0) {
        device->usb_stats.bulk_errors++;
        sdrn_set_error_infof(
            device, SDRN_ERR_IO, SDRN_ERROR_DOMAIN_STREAM, 0,
            SDRN_ERROR_FLAG_NONE, "bulk read returned a negative byte count"
        );
        return SDRN_ERR_IO;
    }
    *out_read = (size_t)transferred;
    if (transferred > 0) {
        device->usb_stats.bulk_bytes += (uint64_t)transferred;
        device->stream_consumed = true;
    }
    return SDRN_OK;
}

sdrn_status sdrn_dvbt_set_pid_filter(
    sdrn_device *device,
    const uint16_t *pids,
    size_t count
) {
    if (!device || !pids) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_DVBT) {
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "DVB-T mode not active"
        );
        return SDRN_ERR_STATE;
    }
    return sdrn_rtl2832_set_pid_filter(device, pids, count);
}

sdrn_status sdrn_dvbt_disable_pid_filter(sdrn_device *device) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->mode != SDRN_MODE_DVBT && !device->frontend_initialized) {
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "DVB-T frontend not active"
        );
        return SDRN_ERR_STATE;
    }
    return sdrn_rtl2832_disable_pid_filter(device);
}