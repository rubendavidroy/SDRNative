/*
 * SDRNative
 * Node.js native binding
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include <node_api.h>

#include <sdrnative.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Handle {
    std::mutex mutex;
    std::condition_variable cv;
    sdrn_device *device = nullptr;
    unsigned active_reads = 0;
    bool closing = false;
};

struct AsyncReadWork {
    napi_env env = nullptr;
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    napi_ref handle_ref = nullptr;
    Handle *handle = nullptr;
    sdrn_device *device = nullptr;
    std::vector<uint8_t> buffer;
    size_t got = 0;
    uint64_t sequence = 0;
    uint32_t timeout_ms = 1000;
    sdrn_status status = SDRN_OK;
    sdrn_error_info error_info{};
    bool has_error_info = false;
};

struct AsyncLockWork {
    napi_env env = nullptr;
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    napi_ref handle_ref = nullptr;
    Handle *handle = nullptr;
    sdrn_device *device = nullptr;
    uint32_t timeout_ms = 5000;
    uint8_t stage = 0;
    sdrn_status status = SDRN_OK;
    sdrn_error_info error_info{};
    bool has_error_info = false;
};

static bool ok(napi_env env, napi_status status, const char *what) {
    if (status == napi_ok) return true;
    const napi_extended_error_info *info = nullptr;
    napi_get_last_error_info(env, &info);
    std::string message = what;
    if (info && info->error_message) {
        message += ": ";
        message += info->error_message;
    }
    napi_throw_error(env, nullptr, message.c_str());
    return false;
}

static napi_value undefined(napi_env env) {
    napi_value v;
    napi_get_undefined(env, &v);
    return v;
}

static napi_value nullv(napi_env env) {
    napi_value v;
    napi_get_null(env, &v);
    return v;
}

static void set(napi_env env, napi_value object, const char *name, napi_value value) {
    napi_set_named_property(env, object, name, value);
}

static napi_value js_bool(napi_env env, bool value) {
    napi_value v;
    napi_get_boolean(env, value, &v);
    return v;
}

static napi_value js_i32(napi_env env, int32_t value) {
    napi_value v;
    napi_create_int32(env, value, &v);
    return v;
}

static napi_value js_u32(napi_env env, uint32_t value) {
    napi_value v;
    napi_create_uint32(env, value, &v);
    return v;
}

static napi_value js_double(napi_env env, double value) {
    napi_value v;
    napi_create_double(env, value, &v);
    return v;
}

static napi_value js_u64_big(napi_env env, uint64_t value) {
    napi_value v;
    napi_create_bigint_uint64(env, value, &v);
    return v;
}

static napi_value js_u64_number(napi_env env, uint64_t value) {
    napi_value v;
    napi_create_double(env, static_cast<double>(value), &v);
    return v;
}

static napi_value js_string(napi_env env, const char *value) {
    napi_value v;
    napi_create_string_utf8(env, value ? value : "", NAPI_AUTO_LENGTH, &v);
    return v;
}

static void populate_error_properties(napi_env env, napi_value err, const sdrn_error_info &info) {
    set(env, err, "name", js_string(env, "SDRNativeError"));
    set(env, err, "status", js_i32(env, info.status));
    set(env, err, "domain", js_i32(env, info.domain));
    set(env, err, "nativeCode", js_i32(env, info.native_code));
    set(env, err, "flags", js_u32(env, info.flags));
    set(env, err, "terminalStream", js_bool(env, (info.flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0));
    set(env, err, "deviceGone", js_bool(env, (info.flags & SDRN_ERROR_FLAG_DEVICE_GONE) != 0));
}

static sdrn_error_info read_error_info(sdrn_device *device, sdrn_status status) {
    sdrn_error_info info{};
    bool valid = false;
    if (device) {
        valid = sdrn_get_last_error_info(device, &info) == SDRN_OK;
    } else {
        valid = sdrn_get_last_global_error_info(&info) == SDRN_OK;
    }
    if (!valid || info.status == SDRN_OK) {
        info = {};
        info.status = static_cast<int32_t>(status);
        std::snprintf(info.message, sizeof(info.message), "%s", sdrn_status_string(status));
    }
    return info;
}

static napi_value make_error(napi_env env, const sdrn_error_info &info) {
    napi_value message = js_string(env, info.message[0] ? info.message : sdrn_status_string(static_cast<sdrn_status>(info.status)));
    napi_value err;
    napi_create_error(env, nullptr, message, &err);
    populate_error_properties(env, err, info);
    return err;
}

static napi_value throw_status(napi_env env, sdrn_status status, sdrn_device *device = nullptr) {
    sdrn_error_info info = read_error_info(device, status);
    napi_throw(env, make_error(env, info));
    return nullptr;
}

static bool get_bool(napi_env env, napi_value value, bool *out) {
    napi_valuetype type;
    if (!ok(env, napi_typeof(env, value, &type), "napi_typeof")) return false;
    if (type != napi_boolean) {
        napi_throw_type_error(env, nullptr, "expected a boolean");
        return false;
    }
    return ok(env, napi_get_value_bool(env, value, out), "napi_get_value_bool");
}

static bool get_u32(napi_env env, napi_value value, uint32_t *out, const char *name) {
    napi_valuetype type;
    if (!ok(env, napi_typeof(env, value, &type), "napi_typeof")) return false;
    if (type != napi_number) {
        std::string message = std::string(name) + " must be a number";
        napi_throw_type_error(env, nullptr, message.c_str());
        return false;
    }
    double d = 0;
    if (!ok(env, napi_get_value_double(env, value, &d), "napi_get_value_double")) return false;
    if (!(d >= 0.0) || d > static_cast<double>(std::numeric_limits<uint32_t>::max()) || d != static_cast<double>(static_cast<uint32_t>(d))) {
        std::string message = std::string(name) + " must be an unsigned 32-bit integer";
        napi_throw_range_error(env, nullptr, message.c_str());
        return false;
    }
    *out = static_cast<uint32_t>(d);
    return true;
}

static bool get_size(napi_env env, napi_value value, size_t *out, const char *name) {
    napi_valuetype type;
    if (!ok(env, napi_typeof(env, value, &type), "napi_typeof")) return false;
    if (type != napi_number) {
        std::string message = std::string(name) + " must be a number";
        napi_throw_type_error(env, nullptr, message.c_str());
        return false;
    }
    double d = 0;
    if (!ok(env, napi_get_value_double(env, value, &d), "napi_get_value_double")) return false;
    if (!(d >= 0.0) || d > static_cast<double>(std::numeric_limits<size_t>::max()) || d != static_cast<double>(static_cast<size_t>(d))) {
        std::string message = std::string(name) + " must be a non-negative integer";
        napi_throw_range_error(env, nullptr, message.c_str());
        return false;
    }
    *out = static_cast<size_t>(d);
    return true;
}

static bool get_u64(napi_env env, napi_value value, uint64_t *out, const char *name) {
    napi_valuetype type;
    if (!ok(env, napi_typeof(env, value, &type), "napi_typeof")) return false;
    if (type == napi_bigint) {
        bool lossless = false;
        if (!ok(env, napi_get_value_bigint_uint64(env, value, out, &lossless), "napi_get_value_bigint_uint64")) return false;
        if (!lossless) {
            std::string message = std::string(name) + " is outside uint64 range";
            napi_throw_range_error(env, nullptr, message.c_str());
            return false;
        }
        return true;
    }
    if (type == napi_number) {
        double d = 0;
        if (!ok(env, napi_get_value_double(env, value, &d), "napi_get_value_double")) return false;
        constexpr double max_safe = 9007199254740991.0;
        if (!(d >= 0.0) || d > max_safe || d != static_cast<double>(static_cast<uint64_t>(d))) {
            std::string message = std::string(name) + " must be a non-negative safe integer or bigint";
            napi_throw_range_error(env, nullptr, message.c_str());
            return false;
        }
        *out = static_cast<uint64_t>(d);
        return true;
    }
    std::string message = std::string(name) + " must be a number or bigint";
    napi_throw_type_error(env, nullptr, message.c_str());
    return false;
}

static bool has(napi_env env, napi_value object, const char *name) {
    bool result = false;
    napi_has_named_property(env, object, name, &result);
    return result;
}

static napi_value named(napi_env env, napi_value object, const char *name) {
    napi_value value;
    napi_get_named_property(env, object, name, &value);
    return value;
}

static bool require_args(napi_env env, size_t argc, size_t min, const char *message) {
    if (argc >= min) return true;
    napi_throw_type_error(env, nullptr, message);
    return false;
}

static Handle *unwrap_handle(napi_env env, napi_value value) {
    void *ptr = nullptr;
    napi_valuetype type;
    if (!ok(env, napi_typeof(env, value, &type), "napi_typeof")) return nullptr;
    if (type != napi_external || !ok(env, napi_get_value_external(env, value, &ptr), "napi_get_value_external")) {
        napi_throw_type_error(env, nullptr, "invalid device handle");
        return nullptr;
    }
    auto *handle = static_cast<Handle *>(ptr);
    if (!handle) {
        napi_throw_error(env, nullptr, "invalid device handle");
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->device || handle->closing) {
        napi_throw_error(env, nullptr, "device is closed");
        return nullptr;
    }
    return handle;
}

static sdrn_device *device_of(Handle *handle) {
    std::lock_guard<std::mutex> lock(handle->mutex);
    return handle->device;
}

static void close_handle(Handle *handle) {
    if (!handle) return;
    sdrn_device *device = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!handle->device || handle->closing) return;
        handle->closing = true;
        device = handle->device;
    }

    if (device && sdrn_async_is_running(device)) {
        (void)sdrn_async_stop(device);
    }

    {
        std::unique_lock<std::mutex> lock(handle->mutex);
        handle->cv.wait(lock, [&] { return handle->active_reads == 0; });
        device = handle->device;
        handle->device = nullptr;
    }
    if (device) sdrn_close(device);
}

static void finalize_handle(napi_env, void *data, void *) {
    auto *handle = static_cast<Handle *>(data);
    close_handle(handle);
    delete handle;
}

static napi_value make_device_info(napi_env env, const sdrn_device_info &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "vid", js_u32(env, c.vid));
    set(env, o, "pid", js_u32(env, c.pid));
    set(env, o, "bus", js_u32(env, c.bus));
    set(env, o, "address", js_u32(env, c.address));
    set(env, o, "manufacturer", js_string(env, c.manufacturer));
    set(env, o, "product", js_string(env, c.product));
    set(env, o, "serial", js_string(env, c.serial));
    return o;
}

static napi_value make_iq_tune(napi_env env, const sdrn_iq_tune_info &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "rfHz", js_u64_number(env, c.rf_hz));
    set(env, o, "requestedSampleRateHz", js_u32(env, c.requested_sample_rate_hz));
    set(env, o, "actualSampleRateHz", js_u32(env, c.actual_sample_rate_hz));
    set(env, o, "bandwidthHz", js_u32(env, c.bandwidth_hz));
    set(env, o, "ifHz", js_u32(env, c.if_hz));
    set(env, o, "loHz", js_u64_number(env, c.lo_hz));
    set(env, o, "tunerPllLocked", js_bool(env, c.tuner_pll_locked != 0));
    return o;
}

static napi_value make_dvbt_tune(napi_env env, const sdrn_dvbt_tune_info &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "rfHz", js_u64_number(env, c.rf_hz));
    set(env, o, "bandwidthHz", js_u32(env, c.bandwidth_hz));
    set(env, o, "ifHz", js_u32(env, c.if_hz));
    set(env, o, "loHz", js_u64_number(env, c.lo_hz));
    set(env, o, "tunerPllLocked", js_bool(env, c.tuner_pll_locked != 0));
    set(env, o, "filterCalCode", js_u32(env, c.filter_cal_code));
    return o;
}

static napi_value make_stream_info(napi_env env, const sdrn_stream_info &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "format", js_i32(env, static_cast<int32_t>(c.format)));
    set(env, o, "sampleRateHz", js_u32(env, c.sample_rate_hz));
    set(env, o, "packetSizeBytes", js_u32(env, c.packet_size_bytes));
    set(env, o, "bytesPerComplexSample", js_u32(env, c.bytes_per_complex_sample));
    return o;
}

static napi_value make_quality(napi_env env, const sdrn_signal_quality &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "fsmStage", js_u32(env, c.fsm_stage));
    set(env, o, "strengthPercent", js_i32(env, c.strength_percent));
    set(env, o, "snrDb", c.snr_valid ? js_double(env, c.snr_db) : nullv(env));
    set(env, o, "ber", c.ber_valid ? js_u32(env, c.ber) : nullv(env));
    set(env, o, "berRaw", js_u32(env, c.ber));
    return o;
}

static napi_value make_usb_stats(napi_env env, const sdrn_usb_stats &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "controlTransfers", js_u64_big(env, c.control_transfers));
    set(env, o, "controlErrors", js_u64_big(env, c.control_errors));
    set(env, o, "controlPipeEvents", js_u64_big(env, c.control_pipe_events));
    set(env, o, "sessionRecoveryAttempts", js_u64_big(env, c.session_recovery_attempts));
    set(env, o, "sessionRecoverySuccesses", js_u64_big(env, c.session_recovery_successes));
    set(env, o, "sessionRecoveryFailures", js_u64_big(env, c.session_recovery_failures));
    set(env, o, "bulkReads", js_u64_big(env, c.bulk_reads));
    set(env, o, "bulkTimeouts", js_u64_big(env, c.bulk_timeouts));
    set(env, o, "bulkErrors", js_u64_big(env, c.bulk_errors));
    set(env, o, "bulkBytes", js_u64_big(env, c.bulk_bytes));
    return o;
}

static napi_value make_usb_recovery_stats(napi_env env, const sdrn_usb_recovery_stats &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "statsVersion", js_u32(env, c.stats_version));
    set(env, o, "controlPipeEvents", js_u64_big(env, c.control_pipe_events));
    set(env, o, "sessionReopenAttempts", js_u64_big(env, c.session_reopen_attempts));
    set(env, o, "sessionReopenSuccesses", js_u64_big(env, c.session_reopen_successes));
    set(env, o, "sessionReopenFailures", js_u64_big(env, c.session_reopen_failures));
    set(env, o, "transactionRetryAttempts", js_u64_big(env, c.transaction_retry_attempts));
    set(env, o, "transactionRetrySuccesses", js_u64_big(env, c.transaction_retry_successes));
    set(env, o, "transactionRetryFailures", js_u64_big(env, c.transaction_retry_failures));
    return o;
}

static napi_value make_async_stats(napi_env env, const sdrn_async_stats &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "statsVersion", js_u32(env, c.stats_version));
    set(env, o, "running", js_bool(env, c.running != 0));
    set(env, o, "queueCurrent", js_u32(env, c.queue_current));
    set(env, o, "queueHighWater", js_u32(env, c.queue_high_water));
    set(env, o, "transferCallbacks", js_u64_big(env, c.transfer_callbacks));
    set(env, o, "transfersCompleted", js_u64_big(env, c.transfers_completed));
    set(env, o, "transferBytes", js_u64_big(env, c.transfer_bytes));
    set(env, o, "transferTimeouts", js_u64_big(env, c.transfer_timeouts));
    set(env, o, "transferErrors", js_u64_big(env, c.transfer_errors));
    set(env, o, "buffersEnqueued", js_u64_big(env, c.buffers_enqueued));
    set(env, o, "buffersDequeued", js_u64_big(env, c.buffers_dequeued));
    set(env, o, "buffersDroppedOldest", js_u64_big(env, c.buffers_dropped_oldest));
    set(env, o, "buffersDroppedNewest", js_u64_big(env, c.buffers_dropped_newest));
    set(env, o, "consumerReads", js_u64_big(env, c.consumer_reads));
    set(env, o, "consumerBytes", js_u64_big(env, c.consumer_bytes));
    set(env, o, "buffersDiscardedOnStop", js_u64_big(env, c.buffers_discarded_on_stop));
    set(env, o, "terminalStatus", js_i32(env, c.terminal_status));
    set(env, o, "terminalNativeCode", js_i32(env, c.terminal_native_code));
    return o;
}

static napi_value make_profile(napi_env env, const sdrn_device_profile &c) {
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "profileVersion", js_u32(env, c.profile_version));
    set(env, o, "featureFlags", js_u32(env, c.feature_flags));
    set(env, o, "tunerFamily", js_i32(env, c.tuner_family));
    set(env, o, "demodFamily", js_i32(env, c.demod_family));
    set(env, o, "maxPidFilters", js_u32(env, c.max_pid_filters));
    set(env, o, "tunerXtalHz", js_u32(env, c.tuner_xtal_hz));
    set(env, o, "demodXtalHz", js_u32(env, c.demod_xtal_hz));

    napi_value rf;
    napi_create_array_with_length(env, std::min<uint32_t>(c.rf_range_count, SDRN_MAX_RF_RANGES), &rf);
    for (uint32_t i = 0; i < std::min<uint32_t>(c.rf_range_count, SDRN_MAX_RF_RANGES); ++i) {
        napi_value r;
        napi_create_object(env, &r);
        set(env, r, "minHz", js_u64_number(env, c.rf_ranges[i].min_hz));
        set(env, r, "maxHz", js_u64_number(env, c.rf_ranges[i].max_hz));
        napi_set_element(env, rf, i, r);
    }
    set(env, o, "rfRanges", rf);

    auto make_rates = [&](uint32_t count, const sdrn_rate_range *ranges) {
        napi_value a;
        uint32_t n = std::min<uint32_t>(count, SDRN_MAX_RATE_RANGES);
        napi_create_array_with_length(env, n, &a);
        for (uint32_t i = 0; i < n; ++i) {
            napi_value r;
            napi_create_object(env, &r);
            set(env, r, "minHz", js_u32(env, ranges[i].min_hz));
            set(env, r, "maxHz", js_u32(env, ranges[i].max_hz));
            napi_set_element(env, a, i, r);
        }
        return a;
    };
    set(env, o, "iqSampleRateRanges", make_rates(c.iq_sample_rate_range_count, c.iq_sample_rate_ranges));
    set(env, o, "iqBandwidthRanges", make_rates(c.iq_bandwidth_range_count, c.iq_bandwidth_ranges));

    napi_value bands;
    uint32_t n = std::min<uint32_t>(c.dvbt_bandwidth_count, SDRN_MAX_DVBT_BANDWIDTHS);
    napi_create_array_with_length(env, n, &bands);
    for (uint32_t i = 0; i < n; ++i) napi_set_element(env, bands, i, js_u32(env, c.dvbt_bandwidth_hz[i]));
    set(env, o, "dvbtBandwidthsHz", bands);
    return o;
}

static napi_value Version(napi_env env, napi_callback_info) {
    return js_string(env, sdrn_version_string());
}

static napi_value AbiVersion(napi_env env, napi_callback_info) {
    return js_u32(env, sdrn_abi_version());
}

static napi_value ListDevices(napi_env env, napi_callback_info) {
    size_t count = 0;
    sdrn_status st = sdrn_list_devices(nullptr, 0, &count);
    if (st != SDRN_OK) return throw_status(env, st);
    std::vector<sdrn_device_info> infos(count);
    size_t total = 0;
    if (count) {
        st = sdrn_list_devices(infos.data(), infos.size(), &total);
        if (st != SDRN_OK) return throw_status(env, st);
    }
    napi_value out;
    napi_create_array_with_length(env, infos.size(), &out);
    for (size_t i = 0; i < infos.size(); ++i) napi_set_element(env, out, static_cast<uint32_t>(i), make_device_info(env, infos[i]));
    return out;
}

static napi_value Open(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    size_t index = 0;
    if (argc >= 1 && !get_size(env, argv[0], &index, "index")) return nullptr;
    sdrn_device *device = nullptr;
    sdrn_status st = sdrn_open(index, &device);
    if (st != SDRN_OK) return throw_status(env, st);
    auto *handle = new Handle();
    handle->device = device;
    napi_value external;
    if (!ok(env, napi_create_external(env, handle, finalize_handle, nullptr, &external), "napi_create_external")) {
        sdrn_close(device);
        delete handle;
        return nullptr;
    }
    return external;
}

static napi_value Close(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 1, "close(handle) requires a handle")) return nullptr;
    void *ptr = nullptr;
    if (!ok(env, napi_get_value_external(env, argv[0], &ptr), "napi_get_value_external")) return nullptr;
    close_handle(static_cast<Handle *>(ptr));
    return undefined(env);
}

#define HANDLE_FN_BEGIN(expected, msg) \
    size_t argc = expected; napi_value argv[expected ? expected : 1]; \
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr); \
    if (!require_args(env, argc, 1, msg)) return nullptr; \
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr; \
    sdrn_device *device = device_of(handle); if (!device) { napi_throw_error(env, nullptr, "device is closed"); return nullptr; }

static napi_value DeviceInfo(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "deviceInfo(handle) requires a handle");
    sdrn_device_info c{};
    sdrn_status st = sdrn_get_device_info(device, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_device_info(env, c);
}

static napi_value DeviceProfile(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "deviceProfile(handle) requires a handle");
    sdrn_device_profile c{};
    sdrn_status st = sdrn_get_device_profile(device, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_profile(env, c);
}

static napi_value GetMode(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "mode(handle) requires a handle");
    return js_i32(env, static_cast<int32_t>(sdrn_get_mode(device)));
}

static napi_value GetState(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "state(handle) requires a handle");
    return js_i32(env, static_cast<int32_t>(sdrn_get_state(device)));
}

static napi_value Stop(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "stop(handle) requires a handle");
    sdrn_status st = sdrn_stop(device);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value StreamInfo(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "streamInfo(handle) requires a handle");
    sdrn_stream_info c{};
    sdrn_status st = sdrn_get_stream_info(device, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_stream_info(env, c);
}

static napi_value USBStats(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "usbStats(handle) requires a handle");
    sdrn_usb_stats c{};
    sdrn_status st = sdrn_get_usb_stats(device, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_usb_stats(env, c);
}

static napi_value USBRecoveryStats(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "usbRecoveryStats(handle) requires a handle");
    sdrn_usb_recovery_stats c{};
    sdrn_status st = sdrn_get_usb_recovery_stats(device, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_usb_recovery_stats(env, c);
}

static napi_value ResetUSBStats(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "resetUsbStats(handle) requires a handle");
    sdrn_status st = sdrn_reset_usb_stats(device);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static bool parse_async_config(napi_env env, napi_value value, sdrn_async_config *out) {
    sdrn_async_config_init(out);
    napi_valuetype type;
    napi_typeof(env, value, &type);
    if (type == napi_undefined || type == napi_null) return true;
    if (type != napi_object) {
        napi_throw_type_error(env, nullptr, "async config must be an object");
        return false;
    }
    if (has(env, value, "transferSizeBytes") && !get_u32(env, named(env, value, "transferSizeBytes"), &out->transfer_size_bytes, "transferSizeBytes")) return false;
    if (has(env, value, "transferCount") && !get_u32(env, named(env, value, "transferCount"), &out->transfer_count, "transferCount")) return false;
    if (has(env, value, "queueDepth") && !get_u32(env, named(env, value, "queueDepth"), &out->queue_depth, "queueDepth")) return false;
    if (has(env, value, "transferTimeoutMs") && !get_u32(env, named(env, value, "transferTimeoutMs"), &out->transfer_timeout_ms, "transferTimeoutMs")) return false;
    if (has(env, value, "overflowPolicy")) {
        uint32_t policy = 0;
        if (!get_u32(env, named(env, value, "overflowPolicy"), &policy, "overflowPolicy")) return false;
        if (policy > 1) {
            napi_throw_range_error(env, nullptr, "overflowPolicy must be 0 (drop-oldest) or 1 (drop-newest)");
            return false;
        }
        out->overflow_policy = static_cast<int32_t>(policy);
    }
    return true;
}

static napi_value AsyncDefaults(napi_env env, napi_callback_info) {
    sdrn_async_config c{};
    sdrn_async_config_init(&c);
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "transferSizeBytes", js_u32(env, c.transfer_size_bytes));
    set(env, o, "transferCount", js_u32(env, c.transfer_count));
    set(env, o, "queueDepth", js_u32(env, c.queue_depth));
    set(env, o, "transferTimeoutMs", js_u32(env, c.transfer_timeout_ms));
    set(env, o, "overflowPolicy", js_i32(env, c.overflow_policy));
    return o;
}

static napi_value AsyncStart(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 1, "asyncStart(handle, config?) requires a handle")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    sdrn_async_config cfg{};
    const sdrn_async_config *cfg_ptr = nullptr;
    if (argc >= 2) {
        if (!parse_async_config(env, argv[1], &cfg)) return nullptr;
        cfg_ptr = &cfg;
    }
    sdrn_status st = sdrn_async_start(device, cfg_ptr);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static void AsyncReadExecute(napi_env, void *data) {
    auto *w = static_cast<AsyncReadWork *>(data);
    w->status = sdrn_async_read(w->device, w->buffer.data(), w->buffer.size(), &w->got, &w->sequence, w->timeout_ms);
    if (w->status != SDRN_OK && w->status != SDRN_ERR_TIMEOUT) {
        w->error_info = read_error_info(w->device, w->status);
        w->has_error_info = true;
    }
    {
        std::lock_guard<std::mutex> lock(w->handle->mutex);
        if (w->handle->active_reads > 0) --w->handle->active_reads;
    }
    w->handle->cv.notify_all();
}

static void AsyncReadComplete(napi_env env, napi_status napi_st, void *data) {
    auto *w = static_cast<AsyncReadWork *>(data);
    if (napi_st != napi_ok) {
        napi_value err;
        napi_create_error(env, nullptr, js_string(env, "async worker failed"), &err);
        napi_reject_deferred(env, w->deferred, err);
    } else if (w->status == SDRN_ERR_TIMEOUT) {
        napi_resolve_deferred(env, w->deferred, nullv(env));
    } else if (w->status != SDRN_OK) {
        sdrn_error_info info = w->has_error_info ? w->error_info : read_error_info(nullptr, w->status);
        napi_reject_deferred(env, w->deferred, make_error(env, info));
    } else {
        napi_value out;
        napi_create_object(env, &out);
        napi_value buffer;
        napi_create_buffer_copy(env, w->got, w->buffer.data(), nullptr, &buffer);
        set(env, out, "data", buffer);
        set(env, out, "sequence", js_u64_big(env, w->sequence));
        napi_resolve_deferred(env, w->deferred, out);
    }
    if (w->handle_ref) napi_delete_reference(env, w->handle_ref);
    if (w->work) napi_delete_async_work(env, w->work);
    delete w;
}

static napi_value AsyncRead(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 1, "asyncRead(handle, size?, timeoutMs?) requires a handle")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    uint32_t size = SDRN_ASYNC_DEFAULT_TRANSFER_SIZE;
    uint32_t timeout = 1000;
    if (argc >= 2 && !get_u32(env, argv[1], &size, "size")) return nullptr;
    if (argc >= 3 && !get_u32(env, argv[2], &timeout, "timeoutMs")) return nullptr;
    if (size == 0 || size > SDRN_ASYNC_MAX_QUEUE_BYTES) {
        napi_throw_range_error(env, nullptr, "size must be between 1 and 64 MiB");
        return nullptr;
    }

    sdrn_device *device = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!handle->device || handle->closing) {
            napi_throw_error(env, nullptr, "device is closed");
            return nullptr;
        }
        if (handle->active_reads != 0) {
            napi_throw_error(env, nullptr, "an async read is already in progress");
            return nullptr;
        }
        ++handle->active_reads;
        device = handle->device;
    }

    auto *w = new AsyncReadWork();
    w->env = env;
    w->handle = handle;
    w->device = device;
    w->buffer.resize(size);
    w->timeout_ms = timeout;

    napi_value promise;
    if (!ok(env, napi_create_promise(env, &w->deferred, &promise), "napi_create_promise") ||
        !ok(env, napi_create_reference(env, argv[0], 1, &w->handle_ref), "napi_create_reference")) {
        {
            std::lock_guard<std::mutex> lock(handle->mutex);
            --handle->active_reads;
        }
        handle->cv.notify_all();
        delete w;
        return nullptr;
    }
    napi_value name = js_string(env, "SDRNativeAsyncRead");
    if (!ok(env, napi_create_async_work(env, nullptr, name, AsyncReadExecute, AsyncReadComplete, w, &w->work), "napi_create_async_work") ||
        !ok(env, napi_queue_async_work(env, w->work), "napi_queue_async_work")) {
        {
            std::lock_guard<std::mutex> lock(handle->mutex);
            --handle->active_reads;
        }
        handle->cv.notify_all();
        napi_delete_reference(env, w->handle_ref);
        if (w->work) napi_delete_async_work(env, w->work);
        delete w;
        return nullptr;
    }
    return promise;
}

static napi_value AsyncStop(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "asyncStop(handle) requires a handle");
    sdrn_status st = sdrn_async_stop(device);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value AsyncRunning(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "asyncRunning(handle) requires a handle");
    return js_bool(env, sdrn_async_is_running(device) != 0);
}

static napi_value AsyncStats(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "asyncStats(handle) requires a handle");
    sdrn_async_stats c{};
    sdrn_status st = sdrn_async_get_stats(device, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_async_stats(env, c);
}

static napi_value ResetAsyncStats(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "resetAsyncStats(handle) requires a handle");
    sdrn_status st = sdrn_async_reset_stats(device);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value IQStart(napi_env env, napi_callback_info info) {
    size_t argc = 4; napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 3, "iqStart(handle, frequencyHz, sampleRateHz, bandwidthHz?)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint64_t frequency = 0; uint32_t rate = 0; uint32_t bandwidth = 0;
    if (!get_u64(env, argv[1], &frequency, "frequencyHz") || !get_u32(env, argv[2], &rate, "sampleRateHz")) return nullptr;
    if (argc >= 4 && !get_u32(env, argv[3], &bandwidth, "bandwidthHz")) return nullptr;
    sdrn_iq_tune_info c{};
    sdrn_status st = sdrn_iq_start(device, frequency, rate, bandwidth, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_iq_tune(env, c);
}

static napi_value IQTune(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 2, "iqTune(handle, frequencyHz)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint64_t frequency = 0; if (!get_u64(env, argv[1], &frequency, "frequencyHz")) return nullptr;
    sdrn_iq_tune_info c{};
    sdrn_status st = sdrn_iq_tune(device, frequency, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_iq_tune(env, c);
}

static napi_value IQSetSampleRate(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 2, "iqSetSampleRate(handle, sampleRateHz)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t rate = 0; if (!get_u32(env, argv[1], &rate, "sampleRateHz")) return nullptr;
    uint32_t actual = 0;
    sdrn_status st = sdrn_iq_set_sample_rate(device, rate, &actual);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return js_u32(env, actual);
}

static napi_value IQSetBandwidth(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 2, "iqSetBandwidth(handle, bandwidthHz)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t bandwidth = 0; if (!get_u32(env, argv[1], &bandwidth, "bandwidthHz")) return nullptr;
    sdrn_iq_tune_info c{};
    sdrn_status st = sdrn_iq_set_bandwidth(device, bandwidth, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_iq_tune(env, c);
}

static napi_value DVBStart(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 3, "dvbtStart(handle, frequencyHz, bandwidthHz)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint64_t frequency = 0; uint32_t bandwidth = 0;
    if (!get_u64(env, argv[1], &frequency, "frequencyHz") || !get_u32(env, argv[2], &bandwidth, "bandwidthHz")) return nullptr;
    sdrn_dvbt_tune_info c{};
    sdrn_status st = sdrn_dvbt_start(device, frequency, bandwidth, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_dvbt_tune(env, c);
}

static napi_value DVBTune(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 3, "dvbtTune(handle, frequencyHz, bandwidthHz)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint64_t frequency = 0; uint32_t bandwidth = 0;
    if (!get_u64(env, argv[1], &frequency, "frequencyHz") || !get_u32(env, argv[2], &bandwidth, "bandwidthHz")) return nullptr;
    sdrn_dvbt_tune_info c{};
    sdrn_status st = sdrn_dvbt_tune(device, frequency, bandwidth, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_dvbt_tune(env, c);
}

static napi_value WaitLock(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 1, "waitLock(handle, timeoutMs?)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t timeout = 4000; if (argc >= 2 && !get_u32(env, argv[1], &timeout, "timeoutMs")) return nullptr;
    uint8_t stage = 0;
    sdrn_status st = sdrn_dvbt_wait_lock(device, timeout, &stage);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return js_u32(env, stage);
}

static void WaitForLockExecute(napi_env, void *data) {
    auto *w = static_cast<AsyncLockWork *>(data);
    w->status = sdrn_dvbt_wait_lock(w->device, w->timeout_ms, &w->stage);
    if (w->status != SDRN_OK) {
        w->error_info = read_error_info(w->device, w->status);
        w->has_error_info = true;
    }
    {
        std::lock_guard<std::mutex> lock(w->handle->mutex);
        if (w->handle->active_reads > 0) --w->handle->active_reads;
    }
    w->handle->cv.notify_all();
}

static void WaitForLockComplete(napi_env env, napi_status napi_st, void *data) {
    auto *w = static_cast<AsyncLockWork *>(data);
    if (napi_st != napi_ok) {
        napi_value err;
        napi_create_error(env, nullptr, js_string(env, "async worker failed"), &err);
        napi_reject_deferred(env, w->deferred, err);
    } else if (w->status != SDRN_OK) {
        napi_reject_deferred(env, w->deferred, make_error(env, w->has_error_info ? w->error_info : read_error_info(nullptr, w->status)));
    } else {
        napi_resolve_deferred(env, w->deferred, js_u32(env, w->stage));
    }
    if (w->handle_ref) napi_delete_reference(env, w->handle_ref);
    if (w->work) napi_delete_async_work(env, w->work);
    delete w;
}

static napi_value WaitForLock(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 1, "waitForLock(handle, timeoutMs?)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    uint32_t timeout = 5000; if (argc >= 2 && !get_u32(env, argv[1], &timeout, "timeoutMs")) return nullptr;
    sdrn_device *device = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!handle->device || handle->closing) { napi_throw_error(env, nullptr, "device is closed"); return nullptr; }
        if (handle->active_reads != 0) { napi_throw_error(env, nullptr, "another async operation is in progress"); return nullptr; }
        ++handle->active_reads;
        device = handle->device;
    }
    auto *w = new AsyncLockWork();
    w->env = env; w->handle = handle; w->device = device; w->timeout_ms = timeout;
    napi_value promise;
    if (!ok(env, napi_create_promise(env, &w->deferred, &promise), "napi_create_promise") ||
        !ok(env, napi_create_reference(env, argv[0], 1, &w->handle_ref), "napi_create_reference")) {
        { std::lock_guard<std::mutex> lock(handle->mutex); --handle->active_reads; }
        handle->cv.notify_all(); delete w; return nullptr;
    }
    napi_value name = js_string(env, "SDRNativeWaitForLock");
    if (!ok(env, napi_create_async_work(env, nullptr, name, WaitForLockExecute, WaitForLockComplete, w, &w->work), "napi_create_async_work") ||
        !ok(env, napi_queue_async_work(env, w->work), "napi_queue_async_work")) {
        { std::lock_guard<std::mutex> lock(handle->mutex); --handle->active_reads; }
        handle->cv.notify_all(); napi_delete_reference(env, w->handle_ref); if (w->work) napi_delete_async_work(env, w->work); delete w; return nullptr;
    }
    return promise;
}

static napi_value SignalQuality(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "signalQuality(handle) requires a handle");
    sdrn_signal_quality c{};
    sdrn_status st = sdrn_dvbt_get_signal_quality(device, &c);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return make_quality(env, c);
}

static napi_value StreamRead(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 1, "streamRead(handle, size?, timeoutMs?)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t size = 256u * 1024u; uint32_t timeout = 1000;
    if (argc >= 2 && !get_u32(env, argv[1], &size, "size")) return nullptr;
    if (argc >= 3 && !get_u32(env, argv[2], &timeout, "timeoutMs")) return nullptr;
    if (size == 0) { napi_throw_range_error(env, nullptr, "size must be positive"); return nullptr; }
    std::vector<uint8_t> data(size); size_t got = 0;
    sdrn_status st = sdrn_stream_read(device, data.data(), data.size(), &got, timeout);
    if (st != SDRN_OK) return throw_status(env, st, device);
    napi_value buffer; napi_create_buffer_copy(env, got, data.data(), nullptr, &buffer); return buffer;
}

static napi_value SetPidFilter(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 2, "setPidFilter(handle, pids)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]); if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    bool is_array = false; napi_is_array(env, argv[1], &is_array);
    if (!is_array) { napi_throw_type_error(env, nullptr, "pids must be an array"); return nullptr; }
    uint32_t length = 0; napi_get_array_length(env, argv[1], &length);
    if (length == 0 || length > SDRN_MAX_PID_FILTER) { napi_throw_range_error(env, nullptr, "pids must contain between 1 and 32 entries"); return nullptr; }
    std::vector<uint16_t> pids(length);
    for (uint32_t i = 0; i < length; ++i) {
        napi_value v; napi_get_element(env, argv[1], i, &v); uint32_t pid = 0;
        if (!get_u32(env, v, &pid, "PID") || pid > 0x1fff) { if (pid > 0x1fff) napi_throw_range_error(env, nullptr, "PID must be at most 0x1fff"); return nullptr; }
        pids[i] = static_cast<uint16_t>(pid);
    }
    sdrn_status st = sdrn_dvbt_set_pid_filter(device, pids.data(), pids.size());
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value DisablePidFilter(napi_env env, napi_callback_info info) {
    HANDLE_FN_BEGIN(1, "disablePidFilter(handle) requires a handle");
    sdrn_status st = sdrn_dvbt_disable_pid_filter(device);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static bool get_buffer(napi_env env, napi_value value, void **data, size_t *length) {
    bool is_buffer = false;
    napi_is_buffer(env, value, &is_buffer);
    if (!is_buffer) { napi_throw_type_error(env, nullptr, "data must be a Buffer"); return false; }
    return ok(env, napi_get_buffer_info(env, value, data, length), "napi_get_buffer_info");
}

static napi_value RawControlRead(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 4, "controlRead(handle, value, index, length)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t value = 0, index = 0, length = 0;
    if (!get_u32(env, argv[1], &value, "value") ||
        !get_u32(env, argv[2], &index, "index") ||
        !get_u32(env, argv[3], &length, "length")) return nullptr;
    std::vector<uint8_t> buffer(length);
    sdrn_status st = sdrn_control_read(device, static_cast<uint16_t>(value), static_cast<uint16_t>(index), buffer.data(), buffer.size());
    if (st != SDRN_OK) return throw_status(env, st, device);
    napi_value out;
    napi_create_buffer_copy(env, buffer.size(), buffer.data(), nullptr, &out);
    return out;
}

static napi_value RawControlWrite(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 4, "controlWrite(handle, value, index, data)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t value = 0, index = 0;
    void *data = nullptr;
    size_t length = 0;
    if (!get_u32(env, argv[1], &value, "value") ||
        !get_u32(env, argv[2], &index, "index") ||
        !get_buffer(env, argv[3], &data, &length)) return nullptr;
    sdrn_status st = sdrn_control_write(device, static_cast<uint16_t>(value), static_cast<uint16_t>(index), data, length);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value RawRegRead(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 3, "regRead(handle, reg, length)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t reg = 0, length = 0;
    if (!get_u32(env, argv[1], &reg, "reg") || !get_u32(env, argv[2], &length, "length")) return nullptr;
    std::vector<uint8_t> buffer(length);
    sdrn_status st = sdrn_reg_read(device, static_cast<uint16_t>(reg), buffer.data(), buffer.size());
    if (st != SDRN_OK) return throw_status(env, st, device);
    napi_value out;
    napi_create_buffer_copy(env, buffer.size(), buffer.data(), nullptr, &out);
    return out;
}

static napi_value RawRegWrite(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 3, "regWrite(handle, reg, data)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t reg = 0;
    void *data = nullptr;
    size_t length = 0;
    if (!get_u32(env, argv[1], &reg, "reg") || !get_buffer(env, argv[2], &data, &length)) return nullptr;
    sdrn_status st = sdrn_reg_write(device, static_cast<uint16_t>(reg), data, length);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value RawDemodRead(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 4, "demodRead(handle, page, reg, length)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t page = 0, reg = 0, length = 0;
    if (!get_u32(env, argv[1], &page, "page") ||
        !get_u32(env, argv[2], &reg, "reg") ||
        !get_u32(env, argv[3], &length, "length")) return nullptr;
    std::vector<uint8_t> buffer(length);
    sdrn_status st = sdrn_demod_read(device, static_cast<uint8_t>(page), static_cast<uint8_t>(reg), buffer.data(), buffer.size());
    if (st != SDRN_OK) return throw_status(env, st, device);
    napi_value out;
    napi_create_buffer_copy(env, buffer.size(), buffer.data(), nullptr, &out);
    return out;
}

static napi_value RawDemodWrite(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 4, "demodWrite(handle, page, reg, data)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t page = 0, reg = 0;
    void *data = nullptr;
    size_t length = 0;
    if (!get_u32(env, argv[1], &page, "page") ||
        !get_u32(env, argv[2], &reg, "reg") ||
        !get_buffer(env, argv[3], &data, &length)) return nullptr;
    sdrn_status st = sdrn_demod_write(device, static_cast<uint8_t>(page), static_cast<uint8_t>(reg), data, length);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value I2CReadReg(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 4, "i2cReadReg(handle, address, reg, length)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t address = 0, reg = 0, length = 0;
    if (!get_u32(env, argv[1], &address, "address") ||
        !get_u32(env, argv[2], &reg, "reg") ||
        !get_u32(env, argv[3], &length, "length")) return nullptr;
    std::vector<uint8_t> buffer(length);
    sdrn_status st = sdrn_i2c_read_reg(device, static_cast<uint8_t>(address), static_cast<uint8_t>(reg), buffer.data(), buffer.size());
    if (st != SDRN_OK) return throw_status(env, st, device);
    napi_value out;
    napi_create_buffer_copy(env, buffer.size(), buffer.data(), nullptr, &out);
    return out;
}

static napi_value I2CWriteReg(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 4, "i2cWriteReg(handle, address, reg, data)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t address = 0, reg = 0;
    void *data = nullptr;
    size_t length = 0;
    if (!get_u32(env, argv[1], &address, "address") ||
        !get_u32(env, argv[2], &reg, "reg") ||
        !get_buffer(env, argv[3], &data, &length)) return nullptr;
    sdrn_status st = sdrn_i2c_write_reg(device, static_cast<uint8_t>(address), static_cast<uint8_t>(reg), data, length);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value I2CReadDirect(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 3, "i2cReadDirect(handle, address, length)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t address = 0, length = 0;
    if (!get_u32(env, argv[1], &address, "address") || !get_u32(env, argv[2], &length, "length")) return nullptr;
    std::vector<uint8_t> buffer(length);
    sdrn_status st = sdrn_i2c_read_direct(device, static_cast<uint8_t>(address), buffer.data(), buffer.size());
    if (st != SDRN_OK) return throw_status(env, st, device);
    napi_value out;
    napi_create_buffer_copy(env, buffer.size(), buffer.data(), nullptr, &out);
    return out;
}

static napi_value I2CWriteDirect(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 3, "i2cWriteDirect(handle, address, data)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    uint32_t address = 0;
    void *data = nullptr;
    size_t length = 0;
    if (!get_u32(env, argv[1], &address, "address") || !get_buffer(env, argv[2], &data, &length)) return nullptr;
    sdrn_status st = sdrn_i2c_write_direct(device, static_cast<uint8_t>(address), data, length);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

static napi_value I2CGate(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (!require_args(env, argc, 2, "i2cGate(handle, open)")) return nullptr;
    Handle *handle = unwrap_handle(env, argv[0]);
    if (!handle) return nullptr;
    sdrn_device *device = device_of(handle);
    bool open = false;
    if (!get_bool(env, argv[1], &open)) return nullptr;
    sdrn_status st = open ? sdrn_i2c_gate_open(device) : sdrn_i2c_gate_close(device);
    if (st != SDRN_OK) return throw_status(env, st, device);
    return undefined(env);
}

struct Export { const char *name; napi_callback fn; };

static napi_value Init(napi_env env, napi_value exports) {
    const Export methods[] = {
        {"version", Version}, {"abiVersion", AbiVersion}, {"listDevices", ListDevices}, {"open", Open}, {"close", Close},
        {"deviceInfo", DeviceInfo}, {"deviceProfile", DeviceProfile}, {"mode", GetMode}, {"state", GetState}, {"stop", Stop},
        {"streamInfo", StreamInfo}, {"usbStats", USBStats}, {"usbRecoveryStats", USBRecoveryStats}, {"resetUsbStats", ResetUSBStats},
        {"asyncDefaults", AsyncDefaults}, {"asyncStart", AsyncStart}, {"asyncRead", AsyncRead}, {"asyncStop", AsyncStop},
        {"asyncRunning", AsyncRunning}, {"asyncStats", AsyncStats}, {"resetAsyncStats", ResetAsyncStats},
        {"iqStart", IQStart}, {"iqTune", IQTune}, {"iqSetSampleRate", IQSetSampleRate}, {"iqSetBandwidth", IQSetBandwidth},
        {"dvbtStart", DVBStart}, {"dvbtTune", DVBTune}, {"waitLock", WaitLock}, {"waitForLock", WaitForLock}, {"signalQuality", SignalQuality},
        {"streamRead", StreamRead}, {"setPidFilter", SetPidFilter}, {"disablePidFilter", DisablePidFilter},
        {"controlRead", RawControlRead}, {"controlWrite", RawControlWrite}, {"regRead", RawRegRead}, {"regWrite", RawRegWrite},
        {"demodRead", RawDemodRead}, {"demodWrite", RawDemodWrite}, {"i2cReadReg", I2CReadReg}, {"i2cWriteReg", I2CWriteReg},
        {"i2cReadDirect", I2CReadDirect}, {"i2cWriteDirect", I2CWriteDirect}, {"i2cGate", I2CGate},
    };
    for (const auto &m : methods) {
        napi_value fn;
        if (!ok(env, napi_create_function(env, m.name, NAPI_AUTO_LENGTH, m.fn, nullptr, &fn), "napi_create_function")) return nullptr;
        set(env, exports, m.name, fn);
    }
    set(env, exports, "ASYNC_MIN_TRANSFER_SIZE", js_u32(env, SDRN_ASYNC_MIN_TRANSFER_SIZE));
    set(env, exports, "ASYNC_MAX_TRANSFER_SIZE", js_u32(env, SDRN_ASYNC_MAX_TRANSFER_SIZE));
    set(env, exports, "ASYNC_MIN_TRANSFER_COUNT", js_u32(env, SDRN_ASYNC_MIN_TRANSFER_COUNT));
    set(env, exports, "ASYNC_MAX_TRANSFER_COUNT", js_u32(env, SDRN_ASYNC_MAX_TRANSFER_COUNT));
    set(env, exports, "ASYNC_MIN_QUEUE_DEPTH", js_u32(env, SDRN_ASYNC_MIN_QUEUE_DEPTH));
    set(env, exports, "ASYNC_MAX_QUEUE_DEPTH", js_u32(env, SDRN_ASYNC_MAX_QUEUE_DEPTH));
    set(env, exports, "ASYNC_MAX_QUEUE_BYTES", js_u32(env, SDRN_ASYNC_MAX_QUEUE_BYTES));
    set(env, exports, "ASYNC_MAX_TRANSFER_POOL_BYTES", js_u32(env, SDRN_ASYNC_MAX_TRANSFER_POOL_BYTES));
    return exports;
}

}

NAPI_MODULE_INIT() {
    return Init(env, exports);
}