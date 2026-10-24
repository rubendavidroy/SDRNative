/*
 * SDRNative
 * libusb backend
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#if defined(SDRNATIVE_HAVE_LIBUSB)

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct libusb_backend_ctx {
    libusb_context *ctx;
    int last_control_error;
    int last_native_error;
} libusb_backend_ctx;

static bool supported(const struct libusb_device_descriptor *d) {
    return d->idVendor == SDRN_REALTEK_VID &&
           (d->idProduct == SDRN_RTL2832_PID_1 || d->idProduct == SDRN_RTL2832_PID_2);
}

static sdrn_status map_error(int code) {
    switch (code) {
        case LIBUSB_SUCCESS: return SDRN_OK;
        case LIBUSB_ERROR_TIMEOUT: return SDRN_ERR_TIMEOUT;
        case LIBUSB_ERROR_ACCESS: return SDRN_ERR_ACCESS;
        case LIBUSB_ERROR_NO_DEVICE: return SDRN_ERR_NOT_FOUND;
        case LIBUSB_ERROR_NOT_SUPPORTED: return SDRN_ERR_UNSUPPORTED;
        case LIBUSB_ERROR_NO_MEM: return SDRN_ERR_NO_MEMORY;
        default: return SDRN_ERR_USB;
    }
}

static void set_usb_error(char *error, size_t cap, int code, const char *prefix) {
    if (!error || cap == 0) return;
    snprintf(error, cap, "%s: %s (%d)", prefix, libusb_error_name(code), code);
}

static void safe_string(
    libusb_device_handle *h, uint8_t index, char *out, size_t cap
) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!index || !h) return;
    unsigned char tmp[SDRN_MAX_STRING] = {0};
    int n = libusb_get_string_descriptor_ascii(h, index, tmp, sizeof(tmp) - 1);
    if (n > 0) {
        size_t copy = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        memcpy(out, tmp, copy);
        out[copy] = '\0';
    }
}

static void fill_info(
    libusb_device *device,
    const struct libusb_device_descriptor *desc,
    libusb_device_handle *maybe_handle,
    sdrn_device_info *info
) {
    memset(info, 0, sizeof(*info));
    info->vid = desc->idVendor;
    info->pid = desc->idProduct;
    info->bus = libusb_get_bus_number(device);
    info->address = libusb_get_device_address(device);

    libusb_device_handle *h = maybe_handle;
    bool temporary = false;
    if (!h && libusb_open(device, &h) == LIBUSB_SUCCESS) temporary = true;
    if (h) {
        safe_string(h, desc->iManufacturer, info->manufacturer, sizeof(info->manufacturer));
        safe_string(h, desc->iProduct, info->product, sizeof(info->product));
        safe_string(h, desc->iSerialNumber, info->serial, sizeof(info->serial));
    }
    if (temporary) libusb_close(h);
}

static void backend_destroy(void *ctx) {
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    if (!b) return;
    if (b->ctx) libusb_exit(b->ctx);
    free(b);
}

static sdrn_status backend_list(
    void *ctx,
    sdrn_device_info *infos,
    size_t capacity,
    size_t *count,
    char *error,
    size_t error_cap
) {
    if (!ctx || !count) return SDRN_ERR_INVALID_ARGUMENT;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(b->ctx, &list);
    b->last_native_error = n < 0 ? (int)n : 0;
    if (n < 0) {
        set_usb_error(error, error_cap, (int)n, "libusb_get_device_list");
        return map_error((int)n);
    }

    size_t total = 0;
    for (ssize_t i = 0; i < n; ++i) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != LIBUSB_SUCCESS) continue;
        if (!supported(&d)) continue;
        if (infos && total < capacity) fill_info(list[i], &d, NULL, &infos[total]);
        ++total;
    }
    libusb_free_device_list(list, 1);
    *count = total;
    return SDRN_OK;
}

static sdrn_status backend_open(
    void *ctx,
    size_t index,
    void **out_handle,
    sdrn_device_info *out_info,
    char *error,
    size_t error_cap
) {
    if (!ctx || !out_handle || !out_info) return SDRN_ERR_INVALID_ARGUMENT;
    *out_handle = NULL;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(b->ctx, &list);
    b->last_native_error = n < 0 ? (int)n : 0;
    if (n < 0) {
        set_usb_error(error, error_cap, (int)n, "libusb_get_device_list");
        return map_error((int)n);
    }

    size_t seen = 0;
    sdrn_status result = SDRN_ERR_NOT_FOUND;
    for (ssize_t i = 0; i < n; ++i) {
        struct libusb_device_descriptor d;
        int rc = libusb_get_device_descriptor(list[i], &d);
        if (rc != LIBUSB_SUCCESS || !supported(&d)) continue;
        if (seen++ != index) continue;

        libusb_device_handle *h = NULL;
        rc = libusb_open(list[i], &h);
        b->last_native_error = rc == LIBUSB_SUCCESS ? 0 : rc;
        if (rc != LIBUSB_SUCCESS) {
            set_usb_error(error, error_cap, rc, "libusb_open");
            result = map_error(rc);
            break;
        }

        fill_info(list[i], &d, h, out_info);
        *out_handle = h;
        result = SDRN_OK;
        break;
    }

    if (result == SDRN_ERR_NOT_FOUND && error && error_cap) {
        snprintf(error, error_cap, "RTL2832U device %zu not found", index);
    }
    libusb_free_device_list(list, 1);
    return result;
}

static void backend_close(void *ctx, void *handle) {
    (void)ctx;
    if (handle) libusb_close((libusb_device_handle *)handle);
}

static sdrn_status backend_claim(
    void *ctx, void *handle, int interface_number,
    char *error, size_t error_cap
) {
    if (!ctx || !handle) return SDRN_ERR_INVALID_ARGUMENT;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    libusb_device_handle *h = (libusb_device_handle *)handle;
    /* NOT_SUPPORTED is fine when auto detach is unavailable */
    (void)libusb_set_auto_detach_kernel_driver(h, 1);
    int rc = libusb_claim_interface(h, interface_number);
    b->last_native_error = rc == LIBUSB_SUCCESS ? 0 : rc;
    if (rc != LIBUSB_SUCCESS) {
        set_usb_error(error, error_cap, rc, "libusb_claim_interface");
        return map_error(rc);
    }
    return SDRN_OK;
}

static void backend_release(void *ctx, void *handle, int interface_number) {
    (void)ctx;
    if (handle) (void)libusb_release_interface((libusb_device_handle *)handle, interface_number);
}

static sdrn_status backend_control(
    void *ctx,
    void *handle,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint8_t *data,
    uint16_t length,
    uint32_t timeout_ms,
    int *transferred,
    char *error,
    size_t error_cap
) {
    if (!ctx || !handle || !transferred) return SDRN_ERR_INVALID_ARGUMENT;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    int rc = libusb_control_transfer(
        (libusb_device_handle *)handle,
        request_type,
        request,
        value,
        index,
        data,
        length,
        timeout_ms
    );
    b->last_control_error = rc < 0 ? rc : LIBUSB_SUCCESS;
    b->last_native_error = rc < 0 ? rc : 0;
    if (rc < 0) {
        set_usb_error(error, error_cap, rc, "libusb_control_transfer");
        return map_error(rc);
    }
    *transferred = rc;
    return SDRN_OK;
}

static bool device_vid_pid_matches(
    const struct libusb_device_descriptor *desc,
    const sdrn_device_info *expected
) {
    if (!desc || !expected) return false;
    return desc->idVendor == expected->vid && desc->idProduct == expected->pid;
}

static bool candidate_serial_matches(
    libusb_device_handle *candidate,
    const struct libusb_device_descriptor *desc,
    const sdrn_device_info *expected
) {
    if (!candidate || !desc || !expected) return false;
    if (!expected->serial[0]) return true;
    char serial[SDRN_MAX_STRING] = {0};
    safe_string(candidate, desc->iSerialNumber, serial, sizeof(serial));
    return serial[0] && strcmp(serial, expected->serial) == 0;
}

static sdrn_status open_matching_device(
    libusb_backend_ctx *b,
    const sdrn_device_info *expected,
    libusb_device_handle **out_handle,
    char *error,
    size_t error_cap
) {
    if (!b || !b->ctx || !expected || !out_handle) return SDRN_ERR_INVALID_ARGUMENT;
    *out_handle = NULL;

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(b->ctx, &list);
    if (n < 0) {
        set_usb_error(error, error_cap, (int)n, "libusb_get_device_list(recover)");
        return map_error((int)n);
    }

    /* usb address is not durable across a Windows session reopen */
    /* serial is authoritative when available */
    sdrn_status result = SDRN_ERR_NOT_FOUND;
    for (ssize_t i = 0; i < n; ++i) {
        struct libusb_device_descriptor d;
        int rc = libusb_get_device_descriptor(list[i], &d);
        if (rc != LIBUSB_SUCCESS || !device_vid_pid_matches(&d, expected)) continue;

        if (!expected->serial[0] &&
            (libusb_get_bus_number(list[i]) != expected->bus ||
             libusb_get_device_address(list[i]) != expected->address)) {
            continue;
        }

        libusb_device_handle *candidate = NULL;
        rc = libusb_open(list[i], &candidate);
        if (rc != LIBUSB_SUCCESS) continue;

        if (!candidate_serial_matches(candidate, &d, expected)) {
            libusb_close(candidate);
            continue;
        }

        *out_handle = candidate;
        result = SDRN_OK;
        break;
    }

    libusb_free_device_list(list, 1);
    if (result != SDRN_OK && error && error_cap) {
        snprintf(
            error, error_cap,
            "RTL2832U %04X:%04X serial=%s reopen failed "
            "(last known bus=%u addr=%u) after control STALL",
            expected->vid, expected->pid,
            expected->serial[0] ? expected->serial : "<none>",
            (unsigned)expected->bus, (unsigned)expected->address
        );
    }
    return result;
}

#define SDRN_WINUSB_RECOVER_ATTEMPTS 20u
#define SDRN_WINUSB_RECOVER_DELAY_MS 25u

static sdrn_status backend_recover_session(
    void *ctx,
    void **handle,
    int interface_number,
    const sdrn_device_info *expected_info,
    char *error,
    size_t error_cap
) {
    if (!ctx || !handle || !*handle || !expected_info) return SDRN_ERR_INVALID_ARGUMENT;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    if (b->last_control_error != LIBUSB_ERROR_PIPE) return SDRN_ERR_UNSUPPORTED;

    /* old WinUSB interface must be released before session reopen */
    /* preserve physical device identity with VID PID and serial */
    /* no usb device reset here */
    libusb_device_handle *old = (libusb_device_handle *)*handle;
    libusb_context *old_ctx = b->ctx;

    (void)libusb_release_interface(old, interface_number);
    libusb_close(old);
    *handle = NULL;
    b->ctx = NULL;
    if (old_ctx) libusb_exit(old_ctx);

    libusb_context *fresh_ctx = NULL;
    int rc = libusb_init(&fresh_ctx);
    if (rc != LIBUSB_SUCCESS) {
        set_usb_error(error, error_cap, rc, "libusb_init(recover)");
        b->last_control_error = rc;
        b->last_native_error = rc;
        return map_error(rc);
    }
    b->ctx = fresh_ctx;

    libusb_device_handle *replacement = NULL;
    sdrn_status last_status = SDRN_ERR_NOT_FOUND;
    char last_error[256] = {0};

    for (unsigned attempt = 0; attempt < SDRN_WINUSB_RECOVER_ATTEMPTS; ++attempt) {
        last_error[0] = '\0';
        last_status = open_matching_device(
            b, expected_info, &replacement, last_error, sizeof(last_error)
        );
        if (last_status == SDRN_OK && replacement) {
            (void)libusb_set_auto_detach_kernel_driver(replacement, 1);
            rc = libusb_claim_interface(replacement, interface_number);
            b->last_native_error = rc == LIBUSB_SUCCESS ? 0 : rc;
            if (rc == LIBUSB_SUCCESS) {
                *handle = replacement;
                b->last_control_error = LIBUSB_SUCCESS;
                if (error && error_cap) error[0] = '\0';
                return SDRN_OK;
            }

            set_usb_error(last_error, sizeof(last_error), rc, "libusb_claim_interface(recover)");
            last_status = map_error(rc);
            libusb_close(replacement);
            replacement = NULL;
        }

        if (attempt + 1u < SDRN_WINUSB_RECOVER_ATTEMPTS) {
            sdrn_sleep_ms(SDRN_WINUSB_RECOVER_DELAY_MS);
        }
    }

    if (error && error_cap) {
        if (last_error[0]) {
            snprintf(error, error_cap, "%s", last_error);
        } else {
            snprintf(
                error, error_cap,
                "RTL2832U %04X:%04X serial=%s reopen failed after control STALL",
                expected_info->vid, expected_info->pid,
                expected_info->serial[0] ? expected_info->serial : "<none>"
            );
        }
    }
    return last_status;
}

static sdrn_status backend_bulk_read(
    void *ctx,
    void *handle,
    uint8_t endpoint,
    uint8_t *data,
    int length,
    uint32_t timeout_ms,
    int *transferred,
    char *error,
    size_t error_cap
) {
    if (!ctx || !handle || !data || !transferred) return SDRN_ERR_INVALID_ARGUMENT;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    int actual = 0;
    int rc = libusb_bulk_transfer(
        (libusb_device_handle *)handle,
        endpoint,
        data,
        length,
        &actual,
        timeout_ms
    );
    *transferred = actual;
    b->last_native_error = rc == LIBUSB_SUCCESS ? 0 : rc;
    if (rc != LIBUSB_SUCCESS) {
        set_usb_error(error, error_cap, rc, "libusb_bulk_transfer");
        return map_error(rc);
    }
    return SDRN_OK;
}


typedef struct libusb_async_state libusb_async_state;

typedef struct libusb_async_slot {
    libusb_async_state *owner;
    struct libusb_transfer *transfer;
    uint8_t *buffer;
    bool submitted;
} libusb_async_slot;

struct libusb_async_state {
    libusb_backend_ctx *backend;
    libusb_device_handle *handle;
    uint8_t endpoint;
    uint32_t transfer_size;
    uint32_t transfer_count;
    uint32_t transfer_timeout_ms;
    sdrn_usb_async_completion_fn callback;
    void *callback_user;
    libusb_async_slot *slots;
    sdrn_mutex *mutex;
    sdrn_thread *thread;
    unsigned active_count;
    bool stopping;
    bool suppress_callbacks;
    bool terminal_notified;
    bool device_gone_observed;
};

static void async_set_stopping(libusb_async_state *state) {
    sdrn_mutex_lock(state->mutex);
    state->stopping = true;
    sdrn_mutex_unlock(state->mutex);
}

static bool async_is_stopping(libusb_async_state *state) {
    bool stopping;
    sdrn_mutex_lock(state->mutex);
    stopping = state->stopping;
    sdrn_mutex_unlock(state->mutex);
    return stopping;
}

static unsigned async_active_count(libusb_async_state *state) {
    unsigned active;
    sdrn_mutex_lock(state->mutex);
    active = state->active_count;
    sdrn_mutex_unlock(state->mutex);
    return active;
}

static void async_note_device_gone(libusb_async_state *state);

static void async_cancel_all(libusb_async_state *state) {
    if (!state || !state->slots) return;
    for (uint32_t i = 0u; i < state->transfer_count; ++i) {
        libusb_async_slot *slot = &state->slots[i];
        if (!slot->transfer) continue;

        sdrn_mutex_lock(state->mutex);
        const bool submitted = slot->submitted;
        sdrn_mutex_unlock(state->mutex);
        if (!submitted) continue;

        int rc = libusb_cancel_transfer(slot->transfer);
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            async_note_device_gone(state);
        }
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND) {
            state->backend->last_native_error = rc;
        }
    }
}

static void async_notify_terminal(
    libusb_async_state *state,
    sdrn_status status,
    int32_t native_code,
    const char *detail
) {
    bool notify = false;
    sdrn_mutex_lock(state->mutex);
    if (!state->terminal_notified && !state->suppress_callbacks) {
        state->terminal_notified = true;
        state->stopping = true;
        notify = true;
    } else {
        state->stopping = true;
    }
    sdrn_mutex_unlock(state->mutex);
    if (notify && state->callback) {
        state->callback(
            state->callback_user, status, native_code, NULL, 0u,
            detail ? detail : "async USB transfer failed"
        );
    }
}

static int transfer_status_native(enum libusb_transfer_status status) {
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED: return LIBUSB_SUCCESS;
        case LIBUSB_TRANSFER_ERROR: return LIBUSB_ERROR_IO;
        case LIBUSB_TRANSFER_TIMED_OUT: return LIBUSB_ERROR_TIMEOUT;
        case LIBUSB_TRANSFER_CANCELLED: return LIBUSB_ERROR_INTERRUPTED;
        case LIBUSB_TRANSFER_STALL: return LIBUSB_ERROR_PIPE;
        case LIBUSB_TRANSFER_NO_DEVICE: return LIBUSB_ERROR_NO_DEVICE;
        case LIBUSB_TRANSFER_OVERFLOW: return LIBUSB_ERROR_OVERFLOW;
        default: return LIBUSB_ERROR_OTHER;
    }
}

static void async_note_device_gone(libusb_async_state *state) {
    if (!state || !state->mutex) return;
    sdrn_mutex_lock(state->mutex);
    state->device_gone_observed = true;
    sdrn_mutex_unlock(state->mutex);
}

static void LIBUSB_CALL backend_async_transfer_cb(struct libusb_transfer *transfer) {
    libusb_async_slot *slot = (libusb_async_slot *)transfer->user_data;
    if (!slot || !slot->owner) return;
    libusb_async_state *state = slot->owner;

    sdrn_mutex_lock(state->mutex);
    if (slot->submitted) {
        slot->submitted = false;
        if (state->active_count > 0u) state->active_count--;
    }
    bool stopping = state->stopping;
    bool suppress = state->suppress_callbacks;
    sdrn_mutex_unlock(state->mutex);

    enum libusb_transfer_status tstatus = transfer->status;
    int native = transfer_status_native(tstatus);
    if (tstatus == LIBUSB_TRANSFER_NO_DEVICE) {
        async_note_device_gone(state);
    }
    if (native != LIBUSB_SUCCESS &&
        !(tstatus == LIBUSB_TRANSFER_CANCELLED && stopping)) {
        state->backend->last_native_error = native;
    }

    if (!suppress && state->callback) {
        if (tstatus == LIBUSB_TRANSFER_COMPLETED) {
            state->callback(
                state->callback_user, SDRN_OK, 0,
                transfer->buffer, (size_t)transfer->actual_length, NULL
            );
        } else if (tstatus == LIBUSB_TRANSFER_TIMED_OUT) {
            state->callback(
                state->callback_user, SDRN_ERR_TIMEOUT, LIBUSB_ERROR_TIMEOUT,
                transfer->buffer, (size_t)transfer->actual_length,
                "libusb async bulk transfer timed out"
            );
        } else if (tstatus != LIBUSB_TRANSFER_CANCELLED || !stopping) {
            char detail[128];
            snprintf(
                detail, sizeof(detail),
                "libusb async bulk transfer: status=%d, %s (%d)",
                (int)tstatus, libusb_error_name(native), native
            );
            async_notify_terminal(state, map_error(native), native, detail);
        }
    }

    if (tstatus != LIBUSB_TRANSFER_COMPLETED &&
        tstatus != LIBUSB_TRANSFER_TIMED_OUT) {
        async_set_stopping(state);
        return;
    }

    sdrn_mutex_lock(state->mutex);
    if (!state->stopping) {
        int rc = libusb_submit_transfer(transfer);
        if (rc == LIBUSB_SUCCESS) {
            slot->submitted = true;
            state->active_count++;
            sdrn_mutex_unlock(state->mutex);
            return;
        }
        state->backend->last_native_error = rc;
        if (rc == LIBUSB_ERROR_NO_DEVICE) state->device_gone_observed = true;
        sdrn_mutex_unlock(state->mutex);
        char detail[128];
        snprintf(
            detail, sizeof(detail), "libusb_submit_transfer(resubmit): %s (%d)",
            libusb_error_name(rc), rc
        );
        async_notify_terminal(state, map_error(rc), rc, detail);
        return;
    }
    sdrn_mutex_unlock(state->mutex);
}

static int backend_async_event_thread(void *opaque) {
    libusb_async_state *state = (libusb_async_state *)opaque;
    if (!state) return 0;

    for (;;) {
        if (async_is_stopping(state)) async_cancel_all(state);
        if (async_active_count(state) == 0u) break;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        int rc = libusb_handle_events_timeout(state->backend->ctx, &tv);
        if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_INTERRUPTED) continue;

        state->backend->last_native_error = rc;
        if (rc == LIBUSB_ERROR_NO_DEVICE) async_note_device_gone(state);
        char detail[128];
        snprintf(
            detail, sizeof(detail), "libusb_handle_events_timeout: %s (%d)",
            libusb_error_name(rc), rc
        );
        async_notify_terminal(state, map_error(rc), rc, detail);
        async_cancel_all(state);
    }
    return 0;
}

static void backend_async_free(libusb_async_state *state) {
    if (!state) return;
    if (state->slots) {
        for (uint32_t i = 0u; i < state->transfer_count; ++i) {
            if (state->slots[i].transfer) libusb_free_transfer(state->slots[i].transfer);
            free(state->slots[i].buffer);
        }
    }
    free(state->slots);
    if (state->mutex) sdrn_mutex_destroy(state->mutex);
    free(state);
}

static void backend_async_drain_without_thread(libusb_async_state *state) {
    async_set_stopping(state);
    async_cancel_all(state);
    while (async_active_count(state) > 0u) {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        int rc = libusb_handle_events_timeout(state->backend->ctx, &tv);
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_INTERRUPTED) {
            state->backend->last_native_error = rc;
            async_cancel_all(state);
            sdrn_sleep_ms(10);
        }
        /* Submitted transfers own their buffers until a completion callback. */
    }
}

static sdrn_status backend_async_bulk_start(
    void *ctx,
    void *handle,
    uint8_t endpoint,
    uint32_t transfer_size_bytes,
    uint32_t transfer_count,
    uint32_t transfer_timeout_ms,
    sdrn_usb_async_completion_fn callback,
    void *callback_user,
    void **out_async_handle,
    char *error,
    size_t error_cap
) {
    if (!ctx || !handle || !callback || !out_async_handle ||
        transfer_size_bytes == 0u || transfer_count == 0u) {
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    *out_async_handle = NULL;
    libusb_backend_ctx *backend = (libusb_backend_ctx *)ctx;

    libusb_async_state *state = (libusb_async_state *)calloc(1, sizeof(*state));
    if (!state) return SDRN_ERR_NO_MEMORY;
    state->backend = backend;
    state->handle = (libusb_device_handle *)handle;
    state->endpoint = endpoint;
    state->transfer_size = transfer_size_bytes;
    state->transfer_count = transfer_count;
    state->transfer_timeout_ms = transfer_timeout_ms;
    state->callback = callback;
    state->callback_user = callback_user;
    state->suppress_callbacks = true;

    sdrn_status st = sdrn_mutex_create(&state->mutex);
    if (st != SDRN_OK) {
        backend_async_free(state);
        return st;
    }
    state->slots = (libusb_async_slot *)calloc(transfer_count, sizeof(*state->slots));
    if (!state->slots) {
        backend_async_free(state);
        return SDRN_ERR_NO_MEMORY;
    }

    for (uint32_t i = 0u; i < transfer_count; ++i) {
        libusb_async_slot *slot = &state->slots[i];
        slot->owner = state;
        slot->buffer = (uint8_t *)malloc(transfer_size_bytes);
        slot->transfer = libusb_alloc_transfer(0);
        if (!slot->buffer || !slot->transfer) {
            if (error && error_cap) snprintf(error, error_cap, "async USB transfer pool allocation failed");
            backend_async_drain_without_thread(state);
            backend_async_free(state);
            return SDRN_ERR_NO_MEMORY;
        }
        libusb_fill_bulk_transfer(
            slot->transfer,
            state->handle,
            endpoint,
            slot->buffer,
            (int)transfer_size_bytes,
            backend_async_transfer_cb,
            slot,
            transfer_timeout_ms
        );
        int rc = libusb_submit_transfer(slot->transfer);
        backend->last_native_error = rc == LIBUSB_SUCCESS ? 0 : rc;
        if (rc != LIBUSB_SUCCESS) {
            if (error && error_cap) set_usb_error(error, error_cap, rc, "libusb_submit_transfer(start)");
            backend_async_drain_without_thread(state);
            backend_async_free(state);
            return map_error(rc);
        }
        slot->submitted = true;
        state->active_count++;
    }

    state->suppress_callbacks = false;
    st = sdrn_thread_create(&state->thread, backend_async_event_thread, state);
    if (st != SDRN_OK) {
        state->suppress_callbacks = true;
        if (error && error_cap) snprintf(error, error_cap, "libusb event thread start failed");
        backend_async_drain_without_thread(state);
        backend_async_free(state);
        return st;
    }

    *out_async_handle = state;
    return SDRN_OK;
}

static sdrn_status backend_async_bulk_stop(
    void *ctx,
    void *async_handle,
    char *error,
    size_t error_cap
) {
    (void)ctx;
    (void)error;
    (void)error_cap;
    libusb_async_state *state = (libusb_async_state *)async_handle;
    if (!state) return SDRN_OK;

    async_set_stopping(state);
    async_cancel_all(state);
    if (state->thread) {
        sdrn_thread_join_destroy(state->thread);
        state->thread = NULL;
    }

    sdrn_mutex_lock(state->mutex);
    const bool device_gone = state->device_gone_observed;
    sdrn_mutex_unlock(state->mutex);
    backend_async_free(state);

    if (device_gone) {
        libusb_backend_ctx *backend = (libusb_backend_ctx *)ctx;
        if (backend) backend->last_native_error = LIBUSB_ERROR_NO_DEVICE;
        if (error && error_cap) {
            snprintf(error, error_cap, "device disconnected while draining async transfers");
        }
        return SDRN_ERR_NOT_FOUND;
    }
    return SDRN_OK;
}

static sdrn_status backend_clear_halt(
    void *ctx,
    void *handle,
    uint8_t endpoint,
    char *error,
    size_t error_cap
) {
    if (!ctx || !handle) return SDRN_ERR_INVALID_ARGUMENT;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    int rc = libusb_clear_halt((libusb_device_handle *)handle, endpoint);
    b->last_native_error = rc == LIBUSB_SUCCESS ? 0 : rc;
    if (rc != LIBUSB_SUCCESS) {
        set_usb_error(error, error_cap, rc, "libusb_clear_halt");
        return map_error(rc);
    }
    return SDRN_OK;
}

static sdrn_status backend_endpoint_info(
    void *ctx,
    void *handle,
    int interface_number,
    uint8_t endpoint,
    uint16_t *max_packet_size,
    bool *is_bulk,
    char *error,
    size_t error_cap
) {
    if (!ctx || !handle || !max_packet_size || !is_bulk) return SDRN_ERR_INVALID_ARGUMENT;
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    *max_packet_size = 0;
    *is_bulk = false;

    libusb_device *dev = libusb_get_device((libusb_device_handle *)handle);
    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    b->last_native_error = rc == LIBUSB_SUCCESS ? 0 : rc;
    if (rc != LIBUSB_SUCCESS) {
        set_usb_error(error, error_cap, rc, "libusb_get_active_config_descriptor");
        return map_error(rc);
    }

    bool found = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting && !found; ++a) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            if (alt->bInterfaceNumber != (uint8_t)interface_number) continue;
            for (uint8_t e = 0; e < alt->bNumEndpoints; ++e) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if (ep->bEndpointAddress == endpoint) {
                    *max_packet_size = ep->wMaxPacketSize;
                    *is_bulk = (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
                    found = true;
                    break;
                }
            }
        }
    }
    libusb_free_config_descriptor(cfg);

    if (!found) {
        if (error && error_cap) snprintf(error, error_cap, "endpoint 0x%02X not found", endpoint);
        return SDRN_ERR_NOT_FOUND;
    }
    return SDRN_OK;
}

static int32_t backend_native_error_code(void *ctx) {
    libusb_backend_ctx *b = (libusb_backend_ctx *)ctx;
    return b ? (int32_t)b->last_native_error : 0;
}

static const sdrn_usb_ops OPS = {
    backend_destroy,
    backend_list,
    backend_open,
    backend_close,
    backend_claim,
    backend_release,
    backend_control,
    backend_bulk_read,
    backend_clear_halt,
    backend_recover_session,
    backend_endpoint_info,
    backend_async_bulk_start,
    backend_async_bulk_stop,
    backend_native_error_code,
};

sdrn_status sdrn_libusb_backend_create(
    const sdrn_usb_ops **out_ops,
    void **out_ctx,
    char *error,
    size_t error_cap
) {
    if (!out_ops || !out_ctx) return SDRN_ERR_INVALID_ARGUMENT;
    *out_ops = NULL;
    *out_ctx = NULL;

    libusb_backend_ctx *b = (libusb_backend_ctx *)calloc(1, sizeof(*b));
    if (!b) return SDRN_ERR_NO_MEMORY;
    int rc = libusb_init(&b->ctx);
    if (rc != LIBUSB_SUCCESS) {
        set_usb_error(error, error_cap, rc, "libusb_init");
        free(b);
        return map_error(rc);
    }
    *out_ops = &OPS;
    *out_ctx = b;
    return SDRN_OK;
}

#else /* SDRNATIVE_HAVE_LIBUSB */

#include <stdio.h>

sdrn_status sdrn_libusb_backend_create(
    const sdrn_usb_ops **out_ops,
    void **out_ctx,
    char *error,
    size_t error_cap
) {
    if (out_ops) *out_ops = NULL;
    if (out_ctx) *out_ctx = NULL;
    if (error && error_cap) {
        snprintf(error, error_cap, "libusb-1.0 support not built");
    }
    return SDRN_ERR_UNSUPPORTED;
}

#endif