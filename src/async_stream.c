/*
 * SDRNative
 * Asynchronous streaming
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sdrn_async_slot {
    uint8_t *data;
    size_t length;
    size_t offset;
    uint64_t sequence;
} sdrn_async_slot;

struct sdrn_async_state {
    sdrn_device *device;
    sdrn_mutex *mutex;
    sdrn_cond *cond;
    void *backend_handle;
    sdrn_async_config config;
    sdrn_async_stats stats;
    sdrn_async_slot *slots;
    uint8_t *storage;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t active_readers;
    uint64_t next_sequence;
    bool running;
    bool stopping;
    sdrn_status terminal_status;
    int32_t terminal_native_code;
    char terminal_error[256];
};

static void init_stats(sdrn_async_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->struct_size = (uint32_t)sizeof(*stats);
    stats->stats_version = SDRN_ASYNC_STATS_VERSION;
}

static uint32_t terminal_error_flags(sdrn_status status) {
    uint32_t flags = SDRN_ERROR_FLAG_TERMINAL_STREAM;
    if (status == SDRN_ERR_NOT_FOUND) flags |= SDRN_ERROR_FLAG_DEVICE_GONE;
    return flags;
}

static void set_device_terminal_error(
    sdrn_device *device,
    sdrn_status status,
    int32_t native_code,
    const char *detail
) {
    if (!device || status == SDRN_OK) return;
    if (status == SDRN_ERR_NOT_FOUND) device->device_gone = true;
    sdrn_set_error_infof(
        device, status, SDRN_ERROR_DOMAIN_STREAM, native_code,
        terminal_error_flags(status),
        "async stream failed: %s",
        detail && detail[0] ? detail : sdrn_status_string(status)
    );
}

void sdrn_async_config_init(sdrn_async_config *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->config_version = SDRN_ASYNC_CONFIG_VERSION;
    config->transfer_size_bytes = SDRN_ASYNC_DEFAULT_TRANSFER_SIZE;
    config->transfer_count = SDRN_ASYNC_DEFAULT_TRANSFER_COUNT;
    config->queue_depth = SDRN_ASYNC_DEFAULT_QUEUE_DEPTH;
    config->transfer_timeout_ms = 0u;
    config->overflow_policy = SDRN_ASYNC_DROP_OLDEST;
}

static sdrn_status normalize_config(
    sdrn_device *device,
    const sdrn_async_config *input,
    sdrn_async_config *out
) {
    sdrn_async_config_init(out);
    if (input) {
        if (input->struct_size != 0u && input->struct_size < sizeof(sdrn_async_config)) {
            sdrn_set_error_infof(
                device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
                SDRN_ERROR_FLAG_NONE,
                "async config struct_size too small (%u < %zu)",
                input->struct_size, sizeof(sdrn_async_config)
            );
            return SDRN_ERR_INVALID_ARGUMENT;
        }
        if (input->config_version != 0u && input->config_version != SDRN_ASYNC_CONFIG_VERSION) {
            sdrn_set_error_infof(
                device, SDRN_ERR_UNSUPPORTED, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
                SDRN_ERROR_FLAG_NONE,
                "unsupported async config version %u", input->config_version
            );
            return SDRN_ERR_UNSUPPORTED;
        }
        if (input->transfer_size_bytes != 0u) out->transfer_size_bytes = input->transfer_size_bytes;
        if (input->transfer_count != 0u) out->transfer_count = input->transfer_count;
        if (input->queue_depth != 0u) out->queue_depth = input->queue_depth;
        out->transfer_timeout_ms = input->transfer_timeout_ms;
        out->overflow_policy = input->overflow_policy;
    }

    if (out->transfer_size_bytes < SDRN_ASYNC_MIN_TRANSFER_SIZE ||
        out->transfer_size_bytes > SDRN_ASYNC_MAX_TRANSFER_SIZE) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "transfer size outside %u..%u bytes",
            SDRN_ASYNC_MIN_TRANSFER_SIZE, SDRN_ASYNC_MAX_TRANSFER_SIZE
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    if (out->transfer_count < SDRN_ASYNC_MIN_TRANSFER_COUNT ||
        out->transfer_count > SDRN_ASYNC_MAX_TRANSFER_COUNT) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "transfer count outside %u..%u",
            SDRN_ASYNC_MIN_TRANSFER_COUNT, SDRN_ASYNC_MAX_TRANSFER_COUNT
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    if (out->queue_depth < SDRN_ASYNC_MIN_QUEUE_DEPTH ||
        out->queue_depth > SDRN_ASYNC_MAX_QUEUE_DEPTH) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "queue depth outside %u..%u",
            SDRN_ASYNC_MIN_QUEUE_DEPTH, SDRN_ASYNC_MAX_QUEUE_DEPTH
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    if (out->overflow_policy != SDRN_ASYNC_DROP_OLDEST &&
        out->overflow_policy != SDRN_ASYNC_DROP_NEWEST) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE, "invalid overflow policy %d",
            out->overflow_policy
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    uint64_t queue_bytes = (uint64_t)out->transfer_size_bytes * (uint64_t)out->queue_depth;
    if (queue_bytes > SDRN_ASYNC_MAX_QUEUE_BYTES) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "async queue allocation %llu bytes exceeds %u-byte limit",
            (unsigned long long)queue_bytes, SDRN_ASYNC_MAX_QUEUE_BYTES
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    uint64_t pool_bytes =
        (uint64_t)out->transfer_size_bytes * (uint64_t)out->transfer_count;
    if (pool_bytes > SDRN_ASYNC_MAX_TRANSFER_POOL_BYTES) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "transfer pool allocation %llu bytes exceeds %u-byte limit",
            (unsigned long long)pool_bytes, SDRN_ASYNC_MAX_TRANSFER_POOL_BYTES
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    return SDRN_OK;
}

static void update_queue_stats(sdrn_async_state *state) {
    state->stats.queue_current = state->count;
    if (state->count > state->stats.queue_high_water) {
        state->stats.queue_high_water = state->count;
    }
}

static void backend_completion(
    void *user,
    sdrn_status status,
    int32_t native_code,
    const uint8_t *data,
    size_t length,
    const char *error
) {
    sdrn_async_state *state = (sdrn_async_state *)user;
    if (!state || !state->mutex) return;

    sdrn_mutex_lock(state->mutex);
    state->stats.transfer_callbacks++;

    if (state->stopping) {
        sdrn_mutex_unlock(state->mutex);
        return;
    }

    if (status == SDRN_ERR_TIMEOUT) {
        state->stats.transfer_timeouts++;
    } else if (status != SDRN_OK) {
        state->stats.transfer_errors++;
        if (state->terminal_status == SDRN_OK) {
            state->terminal_status = status;
            state->terminal_native_code = native_code;
            state->stats.terminal_status = status;
            state->stats.terminal_native_code = native_code;
            state->running = false;
            state->stats.running = 0u;
            if (error && error[0]) {
                size_t n = strlen(error);
                if (n >= sizeof(state->terminal_error)) n = sizeof(state->terminal_error) - 1u;
                memcpy(state->terminal_error, error, n);
                state->terminal_error[n] = '\0';
            } else {
                state->terminal_error[0] = '\0';
            }
        }
        sdrn_cond_broadcast(state->cond);
        sdrn_mutex_unlock(state->mutex);
        return;
    }

    if (length > 0u && data) {
        if (length > state->config.transfer_size_bytes) {
            state->stats.transfer_errors++;
            state->terminal_status = SDRN_ERR_IO;
            state->terminal_native_code = 0;
            state->stats.terminal_status = SDRN_ERR_IO;
            state->stats.terminal_native_code = 0;
            state->running = false;
            state->stats.running = 0u;
            snprintf(
                state->terminal_error, sizeof(state->terminal_error),
                "async backend returned %zu bytes (transfer size %u)",
                length, state->config.transfer_size_bytes
            );
            sdrn_cond_broadcast(state->cond);
            sdrn_mutex_unlock(state->mutex);
            return;
        }

        state->stats.transfers_completed++;
        state->stats.transfer_bytes += (uint64_t)length;

        /* sequence belongs to completed producer buffer */
        /* reserve before overflow handling so either drop policy leaves a visible gap */
        const uint64_t completion_sequence = state->next_sequence++;

        if (state->count == state->config.queue_depth) {
            if (state->config.overflow_policy == SDRN_ASYNC_DROP_NEWEST) {
                state->stats.buffers_dropped_newest++;
                sdrn_mutex_unlock(state->mutex);
                return;
            }
            state->head = (state->head + 1u) % state->config.queue_depth;
            state->count--;
            state->stats.buffers_dropped_oldest++;
        }

        sdrn_async_slot *slot = &state->slots[state->tail];
        memcpy(slot->data, data, length);
        slot->length = length;
        slot->offset = 0u;
        slot->sequence = completion_sequence;
        state->tail = (state->tail + 1u) % state->config.queue_depth;
        state->count++;
        state->stats.buffers_enqueued++;
        update_queue_stats(state);
        sdrn_cond_signal(state->cond);
    }

    sdrn_mutex_unlock(state->mutex);
}

static void free_state(sdrn_async_state *state) {
    if (!state) return;
    if (state->cond) sdrn_cond_destroy(state->cond);
    if (state->mutex) sdrn_mutex_destroy(state->mutex);
    free(state->storage);
    free(state->slots);
    free(state);
}

bool sdrn_async_stream_active(const sdrn_device *dev) {
    if (!dev || !dev->async_lifecycle_mutex) return false;
    sdrn_mutex_lock(dev->async_lifecycle_mutex);
    sdrn_async_state *state = dev->async_state;
    if (!state) {
        sdrn_mutex_unlock(dev->async_lifecycle_mutex);
        return false;
    }
    if (!state->mutex) {
        sdrn_mutex_unlock(dev->async_lifecycle_mutex);
        return false;
    }
    sdrn_mutex_lock(state->mutex);
    bool active = state->running && !state->stopping;
    sdrn_mutex_unlock(state->mutex);
    sdrn_mutex_unlock(dev->async_lifecycle_mutex);
    return active;
}

bool sdrn_async_terminal_snapshot(
    const sdrn_device *dev,
    sdrn_status *out_status,
    int32_t *out_native_code,
    char *out_error,
    size_t error_cap
) {
    if (out_status) *out_status = SDRN_OK;
    if (out_native_code) *out_native_code = 0;
    if (out_error && error_cap > 0u) out_error[0] = '\0';
    if (!dev || !dev->async_lifecycle_mutex) return false;

    sdrn_mutex_lock(dev->async_lifecycle_mutex);
    sdrn_async_state *state = dev->async_state;
    if (!state || !state->mutex) {
        sdrn_mutex_unlock(dev->async_lifecycle_mutex);
        return false;
    }
    sdrn_mutex_lock(state->mutex);
    const sdrn_status terminal = state->terminal_status;
    const int32_t native = state->terminal_native_code;
    if (out_error && error_cap > 0u && state->terminal_error[0]) {
        snprintf(out_error, error_cap, "%s", state->terminal_error);
    }
    sdrn_mutex_unlock(state->mutex);
    sdrn_mutex_unlock(dev->async_lifecycle_mutex);

    if (terminal == SDRN_OK) return false;
    if (out_status) *out_status = terminal;
    if (out_native_code) *out_native_code = native;
    return true;
}

sdrn_status sdrn_async_start(
    sdrn_device *device,
    const sdrn_async_config *config
) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (device->device_gone) {
        sdrn_set_error_infof(
            device, SDRN_ERR_NOT_FOUND, SDRN_ERROR_DOMAIN_STREAM, 0,
            SDRN_ERROR_FLAG_TERMINAL_STREAM | SDRN_ERROR_FLAG_DEVICE_GONE,
            "device disconnected; reopen the handle"
        );
        return SDRN_ERR_NOT_FOUND;
    }
    if (!device->async_lifecycle_mutex) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INTERNAL, SDRN_ERROR_DOMAIN_INTERNAL, 0,
            SDRN_ERROR_FLAG_NONE, "async mutex unavailable"
        );
        return SDRN_ERR_INTERNAL;
    }

    sdrn_mutex_lock(device->async_lifecycle_mutex);
    const bool already_active = device->async_state != NULL;
    sdrn_mutex_unlock(device->async_lifecycle_mutex);
    if (already_active) {
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "async stream already active or stopping"
        );
        return SDRN_ERR_STATE;
    }
    if ((device->mode != SDRN_MODE_DVBT && device->mode != SDRN_MODE_IQ) ||
        !device->ts_configured) {
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "no streaming mode active"
        );
        return SDRN_ERR_STATE;
    }
    if (!device->usb || !device->usb->async_bulk_start || !device->usb->async_bulk_stop) {
        sdrn_set_error_infof(
            device, SDRN_ERR_UNSUPPORTED, SDRN_ERROR_DOMAIN_STREAM, 0,
            SDRN_ERROR_FLAG_NONE, "USB backend does not support async bulk transfers"
        );
        return SDRN_ERR_UNSUPPORTED;
    }

    sdrn_async_config resolved;
    sdrn_status st = normalize_config(device, config, &resolved);
    if (st != SDRN_OK) return st;

    sdrn_async_state *state = (sdrn_async_state *)calloc(1, sizeof(*state));
    if (!state) return SDRN_ERR_NO_MEMORY;
    state->device = device;
    state->config = resolved;
    state->terminal_status = SDRN_OK;
    init_stats(&state->stats);
    state->stats.running = 1u;
    state->running = true;

    st = sdrn_mutex_create(&state->mutex);
    if (st != SDRN_OK) {
        free_state(state);
        return st;
    }
    st = sdrn_cond_create(&state->cond);
    if (st != SDRN_OK) {
        free_state(state);
        return st;
    }

    state->slots = (sdrn_async_slot *)calloc(resolved.queue_depth, sizeof(*state->slots));
    uint64_t queue_bytes = (uint64_t)resolved.queue_depth * (uint64_t)resolved.transfer_size_bytes;
    state->storage = (uint8_t *)malloc((size_t)queue_bytes);
    if (!state->slots || !state->storage) {
        free_state(state);
        return SDRN_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0u; i < resolved.queue_depth; ++i) {
        state->slots[i].data = state->storage + ((size_t)i * resolved.transfer_size_bytes);
    }

    sdrn_mutex_lock(device->async_lifecycle_mutex);
    if (device->async_state) {
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        free_state(state);
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "async stream started concurrently"
        );
        return SDRN_ERR_STATE;
    }
    device->async_state = state;
    device->last_async_stats_valid = false;
    init_stats(&device->last_async_stats);

    /* new async session needs a fresh endpoint lifetime after any armed transfer */
    /* pending WinUSB reads count even when no bytes completed */
    if (device->stream_transport_armed) {
        device->async_state = NULL;
        st = sdrn_stream_reset_endpoint(device);
        device->async_state = state;
        if (st != SDRN_OK) {
            device->async_state = NULL;
            sdrn_mutex_unlock(device->async_lifecycle_mutex);
            free_state(state);
            return st;
        }
    }

    char err[256] = {0};
    /* armed bulk transfers require a later quiescence boundary */
    device->stream_transport_armed = true;
    st = device->usb->async_bulk_start(
        device->usb_ctx,
        device->usb_handle,
        SDRN_TS_ENDPOINT,
        resolved.transfer_size_bytes,
        resolved.transfer_count,
        resolved.transfer_timeout_ms,
        backend_completion,
        state,
        &state->backend_handle,
        err,
        sizeof(err)
    );
    if (st != SDRN_OK) {
        device->async_state = NULL;
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        /* keep transport armed after partial backend failure */
        /* next retry or native stop owns endpoint normalization */
        sdrn_set_error_infof(
            device, st, SDRN_ERROR_DOMAIN_STREAM, sdrn_usb_native_error(device),
            SDRN_ERROR_FLAG_NONE, "async stream start failed: %s",
            err[0] ? err : sdrn_status_string(st)
        );
        free_state(state);
        return st;
    }

    sdrn_mutex_unlock(device->async_lifecycle_mutex);

    sdrn_logf(
        device, SDRN_LOG_INFO,
        "async started: %u-byte transfers x %u, queue=%u, overflow=%s",
        resolved.transfer_size_bytes, resolved.transfer_count, resolved.queue_depth,
        resolved.overflow_policy == SDRN_ASYNC_DROP_OLDEST ? "drop-oldest" : "drop-newest"
    );
    return SDRN_OK;
}

static void async_reader_leave(sdrn_async_state *state) {
    if (!state || !state->mutex) return;
    sdrn_mutex_lock(state->mutex);
    if (state->active_readers > 0u) state->active_readers--;
    sdrn_cond_broadcast(state->cond);
    sdrn_mutex_unlock(state->mutex);
}

sdrn_status sdrn_async_read(
    sdrn_device *device,
    void *buffer,
    size_t capacity,
    size_t *out_read,
    uint64_t *out_sequence,
    uint32_t timeout_ms
) {
    if (!device || !buffer || capacity == 0u || !out_read) return SDRN_ERR_INVALID_ARGUMENT;
    *out_read = 0u;
    if (out_sequence) *out_sequence = 0u;

    if (!device->async_lifecycle_mutex) {
        sdrn_set_error_infof(
            device, SDRN_ERR_INTERNAL, SDRN_ERROR_DOMAIN_INTERNAL, 0,
            SDRN_ERROR_FLAG_NONE, "async mutex unavailable"
        );
        return SDRN_ERR_INTERNAL;
    }

    /* pin async state for full read call */
    /* stop may interrupt one blocked consumer from another thread */
    sdrn_mutex_lock(device->async_lifecycle_mutex);
    sdrn_async_state *state = device->async_state;
    if (!state || !state->mutex) {
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "async stream not active"
        );
        return SDRN_ERR_STATE;
    }

    sdrn_mutex_lock(state->mutex);
    if (state->stopping) {
        sdrn_mutex_unlock(state->mutex);
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "async stream stopping"
        );
        return SDRN_ERR_STATE;
    }
    if (state->active_readers != 0u) {
        sdrn_mutex_unlock(state->mutex);
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE,
            "only one async reader allowed per device"
        );
        return SDRN_ERR_STATE;
    }
    state->active_readers = 1u;
    sdrn_mutex_unlock(state->mutex);
    sdrn_mutex_unlock(device->async_lifecycle_mutex);

    sdrn_status result = SDRN_OK;
    bool report_terminal = false;
    sdrn_status terminal = SDRN_OK;
    int32_t terminal_native = 0;
    char terminal_detail[256] = {0};

    sdrn_mutex_lock(state->mutex);
    const uint64_t deadline = timeout_ms == SDRN_ASYNC_WAIT_INFINITE
        ? UINT64_MAX
        : sdrn_now_ms() + (uint64_t)timeout_ms;

    while (state->count == 0u && state->terminal_status == SDRN_OK && !state->stopping) {
        if (timeout_ms == 0u) {
            result = SDRN_ERR_TIMEOUT;
            goto read_done_locked;
        }
        uint32_t wait_ms = SDRN_ASYNC_WAIT_INFINITE;
        if (deadline != UINT64_MAX) {
            uint64_t now = sdrn_now_ms();
            if (now >= deadline) {
                result = SDRN_ERR_TIMEOUT;
                goto read_done_locked;
            }
            uint64_t remaining = deadline - now;
            wait_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
        }
        int wr = sdrn_cond_wait_ms(state->cond, state->mutex, wait_ms);
        if (wr == 1) {
            result = SDRN_ERR_TIMEOUT;
            goto read_done_locked;
        }
        if (wr < 0) {
            goto read_wait_error_locked;
        }
    }

    if (state->count == 0u && state->terminal_status != SDRN_OK) {
        terminal = state->terminal_status;
        terminal_native = state->terminal_native_code;
        snprintf(terminal_detail, sizeof(terminal_detail), "%s", state->terminal_error);
        result = terminal;
        report_terminal = true;
        goto read_done_locked;
    }

    if (state->count == 0u) {
        result = SDRN_ERR_STATE;
        goto read_done_locked;
    }

    sdrn_async_slot *slot = &state->slots[state->head];
    size_t available = slot->length - slot->offset;
    size_t copy = capacity < available ? capacity : available;
    memcpy(buffer, slot->data + slot->offset, copy);
    slot->offset += copy;
    if (out_sequence) *out_sequence = slot->sequence;
    *out_read = copy;
    state->stats.consumer_reads++;
    state->stats.consumer_bytes += (uint64_t)copy;

    if (slot->offset == slot->length) {
        slot->length = 0u;
        slot->offset = 0u;
        state->head = (state->head + 1u) % state->config.queue_depth;
        state->count--;
        state->stats.buffers_dequeued++;
        update_queue_stats(state);
    }
    goto read_done_locked;

read_wait_error_locked:
    sdrn_mutex_unlock(state->mutex);
    async_reader_leave(state);
    sdrn_set_error_infof(
        device, SDRN_ERR_INTERNAL, SDRN_ERROR_DOMAIN_STREAM, 0,
        SDRN_ERROR_FLAG_NONE, "async queue wait failed"
    );
    return SDRN_ERR_INTERNAL;

read_done_locked:
    sdrn_mutex_unlock(state->mutex);
    if (report_terminal) {
        set_device_terminal_error(device, terminal, terminal_native, terminal_detail);
    }
    async_reader_leave(state);
    return result;
}

sdrn_status sdrn_async_stop_internal(sdrn_device *device, bool preserve_error) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (!device->async_lifecycle_mutex) return SDRN_ERR_INTERNAL;

    sdrn_mutex_lock(device->async_lifecycle_mutex);
    sdrn_async_state *state = device->async_state;
    if (!state) {
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        return SDRN_OK;
    }

    char primary_error[sizeof(device->last_error)];
    sdrn_error_info primary_info = device->last_error_info;
    if (preserve_error) {
        snprintf(primary_error, sizeof(primary_error), "%s", device->last_error);
    } else {
        primary_error[0] = '\0';
    }

    sdrn_mutex_lock(state->mutex);
    if (state->stopping) {
        sdrn_mutex_unlock(state->mutex);
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "async stream already stopping"
        );
        return SDRN_ERR_STATE;
    }
    state->stopping = true;
    state->stats.running = 0u;
    sdrn_cond_broadcast(state->cond);
    sdrn_mutex_unlock(state->mutex);
    sdrn_mutex_unlock(device->async_lifecycle_mutex);

    char err[256] = {0};
    sdrn_status st = SDRN_OK;
    if (state->backend_handle && device->usb && device->usb->async_bulk_stop) {
        st = device->usb->async_bulk_stop(
            device->usb_ctx, state->backend_handle, err, sizeof(err)
        );
        state->backend_handle = NULL;
    }

    /* async_bulk_stop() ends backend callback lifetime */
    /* wait for public consumer before freeing queue state */
    /* lifecycle mutex blocks a new reader while state mutex is released */
    sdrn_mutex_lock(device->async_lifecycle_mutex);
    sdrn_mutex_lock(state->mutex);
    while (state->active_readers != 0u) {
        int wr = sdrn_cond_wait_ms(
            state->cond, state->mutex, SDRN_ASYNC_WAIT_INFINITE
        );
        if (wr < 0) {
            if (st == SDRN_OK) st = SDRN_ERR_INTERNAL;
            break;
        }
    }

    state->running = false;
    state->stats.running = 0u;
    state->stats.buffers_discarded_on_stop += state->count;
    state->count = 0u;
    state->head = 0u;
    state->tail = 0u;
    update_queue_stats(state);
    device->last_async_stats = state->stats;
    device->last_async_stats_valid = true;
    if (state->stats.transfer_bytes > 0u) device->stream_consumed = true;
    sdrn_mutex_unlock(state->mutex);

    if (device->async_state == state) device->async_state = NULL;
    free_state(state);
    sdrn_mutex_unlock(device->async_lifecycle_mutex);

    if (st != SDRN_OK) {
        uint32_t flags = SDRN_ERROR_FLAG_NONE;
        if (st == SDRN_ERR_NOT_FOUND) {
            device->device_gone = true;
            flags = SDRN_ERROR_FLAG_TERMINAL_STREAM | SDRN_ERROR_FLAG_DEVICE_GONE;
        }
        sdrn_set_error_infof(
            device, st, SDRN_ERROR_DOMAIN_STREAM, sdrn_usb_native_error(device),
            flags, "async stream stop failed: %s",
            err[0] ? err : sdrn_status_string(st)
        );
        return st;
    }

    if (preserve_error && primary_error[0] != '\0') {
        snprintf(device->last_error, sizeof(device->last_error), "%s", primary_error);
        device->last_error_info = primary_info;
    }

    sdrn_logf(device, SDRN_LOG_INFO, "async stream stopped");
    return SDRN_OK;
}

sdrn_status sdrn_async_stop(sdrn_device *device) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;

    sdrn_status terminal = SDRN_OK;
    int32_t terminal_native = 0;
    char terminal_error[256] = {0};
    (void)sdrn_async_terminal_snapshot(
        device, &terminal, &terminal_native, terminal_error, sizeof(terminal_error)
    );

    sdrn_status stop_st = sdrn_async_stop_internal(device, false);
    if (terminal != SDRN_OK) {
        /* keep first terminal stream error through cleanup */
        set_device_terminal_error(device, terminal, terminal_native, terminal_error);
        return terminal;
    }
    return stop_st;
}

int sdrn_async_is_running(const sdrn_device *device) {
    return sdrn_async_stream_active(device) ? 1 : 0;
}

sdrn_status sdrn_async_get_stats(
    const sdrn_device *device,
    sdrn_async_stats *out_stats
) {
    if (!device || !out_stats) return SDRN_ERR_INVALID_ARGUMENT;
    if (!device->async_lifecycle_mutex) return SDRN_ERR_INTERNAL;
    sdrn_mutex_lock(device->async_lifecycle_mutex);
    sdrn_async_state *state = device->async_state;
    if (state) {
        sdrn_mutex_lock(state->mutex);
        *out_stats = state->stats;
        sdrn_mutex_unlock(state->mutex);
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        return SDRN_OK;
    }
    if (device->last_async_stats_valid) {
        *out_stats = device->last_async_stats;
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        return SDRN_OK;
    }
    init_stats(out_stats);
    sdrn_mutex_unlock(device->async_lifecycle_mutex);
    return SDRN_OK;
}

sdrn_status sdrn_async_reset_stats(sdrn_device *device) {
    if (!device) return SDRN_ERR_INVALID_ARGUMENT;
    if (!device->async_lifecycle_mutex) return SDRN_ERR_INTERNAL;
    sdrn_mutex_lock(device->async_lifecycle_mutex);
    if (device->async_state) {
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        sdrn_set_error_infof(
            device, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "stop async streaming before resetting stats"
        );
        return SDRN_ERR_STATE;
    }
    init_stats(&device->last_async_stats);
    device->last_async_stats_valid = true;
    sdrn_mutex_unlock(device->async_lifecycle_mutex);
    return SDRN_OK;
}

#if defined(SDRNATIVE_TEST_HOOKS)
sdrn_status sdrn_async_test_set_next_sequence(sdrn_device *device, uint64_t sequence) {
    if (!device || !device->async_lifecycle_mutex) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_mutex_lock(device->async_lifecycle_mutex);
    sdrn_async_state *state = device->async_state;
    if (!state) {
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        return SDRN_ERR_STATE;
    }
    sdrn_mutex_lock(state->mutex);
    if (state->active_readers != 0u || state->count != 0u || state->stopping) {
        sdrn_mutex_unlock(state->mutex);
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        return SDRN_ERR_STATE;
    }
    state->next_sequence = sequence;
    sdrn_mutex_unlock(state->mutex);
    sdrn_mutex_unlock(device->async_lifecycle_mutex);
    return SDRN_OK;
}

uint32_t sdrn_async_test_active_readers(const sdrn_device *device) {
    if (!device || !device->async_lifecycle_mutex) return 0u;
    sdrn_mutex_lock(device->async_lifecycle_mutex);
    sdrn_async_state *state = device->async_state;
    if (!state) {
        sdrn_mutex_unlock(device->async_lifecycle_mutex);
        return 0u;
    }
    sdrn_mutex_lock(state->mutex);
    uint32_t readers = state->active_readers;
    sdrn_mutex_unlock(state->mutex);
    sdrn_mutex_unlock(device->async_lifecycle_mutex);
    return readers;
}
#endif