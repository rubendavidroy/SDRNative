/*
 * SDRNative
 * Mock USB backend
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "mock_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* mock target compiles core directly for backend injection */
/* libusb factory stays unsupported here */
sdrn_status sdrn_libusb_backend_create(
    const sdrn_usb_ops **out_ops,
    void **out_ctx,
    char *error,
    size_t error_cap
) {
    if (out_ops) *out_ops = NULL;
    if (out_ctx) *out_ctx = NULL;
    if (error && error_cap) {
        snprintf(error, error_cap, "mock backend does not use libusb");
    }
    return SDRN_ERR_UNSUPPORTED;
}

typedef struct mock_ctx {
    uint8_t regs[65536];
    uint8_t demod[8][512];
    uint8_t tuner[256];
    unsigned tuner_reg_write_count[256];
    uint8_t continuity;
    bool endpoint_reset_armed;
    bool stale_iq_fifo;
    bool halt_needs_clear;
    bool stream_endpoint_active;
    bool require_endpoint_quiesce_before_control;
    unsigned endpoint_quiesce_count;
    unsigned clear_halt_calls;
    sdrn_status clear_halt_status;
    int32_t clear_halt_native_code;
    unsigned recover_session_calls;
    bool recoverable_pipe_enabled;
    bool recoverable_pipe_pending;
    bool recoverable_pipe_injected;
    bool recoverable_pipe_retry_sticky;
    bool tuner_gate_open;
    unsigned tuner_gate_open_count;
    unsigned tuner_gate_close_count;
    unsigned compact_i2c_write_count;
    unsigned direct_i2c_write_count;
    unsigned direct_i2c_read_count;
    uint8_t direct_i2c_pointer;
    bool async_fault_enabled;
    sdrn_status async_fault_status;
    int32_t async_fault_native_code;
    uint32_t async_fault_delay_ms;
    unsigned async_fault_after_successes;
    char async_fault_detail[128];
    sdrn_status async_stop_status;
    int32_t async_stop_native_code;
    bool async_silence_enabled;
    sdrn_mutex *async_silence_mutex;
    bool sync_zero_timeout_enabled;
    int32_t last_native_error;
    bool open;
    bool claimed;
} mock_ctx;

static void destroy(void *opaque) {
    mock_ctx *m = (mock_ctx *)opaque;
    if (!m) return;
    if (m->async_silence_mutex) {
        sdrn_mutex_destroy(m->async_silence_mutex);
        m->async_silence_mutex = NULL;
    }
    free(m);
}

static bool mock_async_silence_get(mock_ctx *m) {
    if (!m || !m->async_silence_mutex) return false;
    sdrn_mutex_lock(m->async_silence_mutex);
    const bool enabled = m->async_silence_enabled;
    sdrn_mutex_unlock(m->async_silence_mutex);
    return enabled;
}

static void mock_async_silence_set(mock_ctx *m, bool enabled) {
    if (!m || !m->async_silence_mutex) return;
    sdrn_mutex_lock(m->async_silence_mutex);
    m->async_silence_enabled = enabled;
    sdrn_mutex_unlock(m->async_silence_mutex);
}

static sdrn_status list(
    void *opaque, sdrn_device_info *infos, size_t capacity, size_t *count,
    char *error, size_t error_cap
) {
    (void)opaque; (void)error; (void)error_cap;
    if (!count) return SDRN_ERR_INVALID_ARGUMENT;
    *count = 1;
    if (infos && capacity) {
        memset(infos, 0, sizeof(*infos));
        infos[0].vid = SDRN_REALTEK_VID;
        infos[0].pid = SDRN_RTL2832_PID_2;
        snprintf(infos[0].manufacturer, sizeof(infos[0].manufacturer), "%s", "SDRNative Test");
        snprintf(infos[0].product, sizeof(infos[0].product), "%s", "Mock RTL2832U + R820T2");
        snprintf(infos[0].serial, sizeof(infos[0].serial), "%s", "MOCK0001");
    }
    return SDRN_OK;
}

static sdrn_status open_dev(
    void *opaque, size_t index, void **out_handle, sdrn_device_info *out_info,
    char *error, size_t error_cap
) {
    mock_ctx *m = (mock_ctx *)opaque;
    if (index != 0) {
        if (error && error_cap) snprintf(error, error_cap, "mock device not found");
        return SDRN_ERR_NOT_FOUND;
    }
    m->open = true;
    *out_handle = m;
    size_t count = 0;
    return list(opaque, out_info, 1, &count, error, error_cap);
}

static void close_dev(void *opaque, void *handle) {
    (void)handle;
    ((mock_ctx *)opaque)->open = false;
}

static sdrn_status claim(
    void *opaque, void *handle, int interface_number,
    char *error, size_t error_cap
) {
    (void)handle; (void)error; (void)error_cap;
    if (interface_number != 0) return SDRN_ERR_INVALID_ARGUMENT;
    ((mock_ctx *)opaque)->claimed = true;
    return SDRN_OK;
}

static void release(void *opaque, void *handle, int interface_number) {
    (void)handle; (void)interface_number;
    ((mock_ctx *)opaque)->claimed = false;
}

static void mem_read(uint8_t *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
}

static void mem_write(uint8_t *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
}

static sdrn_status control(
    void *opaque, void *handle,
    uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index,
    uint8_t *data, uint16_t length, uint32_t timeout_ms,
    int *transferred, char *error, size_t error_cap
) {
    (void)handle; (void)request; (void)timeout_ms; (void)error; (void)error_cap;
    mock_ctx *m = (mock_ctx *)opaque;
    bool input = (request_type & 0x80u) != 0;

    if (m->require_endpoint_quiesce_before_control && m->stream_endpoint_active && !input) {
        const bool endpoint_reset =
            index == SDRN_CMD_USB_WR &&
            value == SDRN_USB_EPA_CTL &&
            length >= 2u && data &&
            data[0] == 0x10u && data[1] == 0x02u;
        if (!endpoint_reset) {
            if (error && error_cap) {
                snprintf(
                    error, error_cap,
                    "mock control PIPE: endpoint 0x81 still active"
                );
            }
            *transferred = 0;
            return SDRN_ERR_USB;
        }
    }

    if (m->recoverable_pipe_enabled && m->recoverable_pipe_pending) {
        if (error && error_cap) {
            snprintf(error, error_cap, "mock control PIPE persists until session reopen");
        }
        *transferred = 0;
        return SDRN_ERR_USB;
    }

    if ((index & 0xFFF0u) == SDRN_CMD_DEMOD_WR ||
        (index & 0xFFF0u) == SDRN_CMD_DEMOD_RD) {
        uint8_t page = (uint8_t)(index & 0x0Fu);
        uint8_t reg = (uint8_t)(value >> 8);
        if (page >= 8 || (size_t)reg + length > 512) return SDRN_ERR_IO;
        if (input) {
            mem_read(data, &m->demod[page][reg], length);
        } else {
            if (m->recoverable_pipe_enabled && !m->recoverable_pipe_injected &&
                page == SDRN_I2C_GATE_PAGE && reg == SDRN_I2C_GATE_REG &&
                length >= 1u && data[0] == SDRN_I2C_GATE_CLOSE) {
                m->recoverable_pipe_injected = true;
                m->recoverable_pipe_pending = true;
                if (error && error_cap) {
                    snprintf(error, error_cap, "mock Windows demod control PIPE");
                }
                *transferred = 0;
                return SDRN_ERR_USB;
            }
            mem_write(&m->demod[page][reg], data, length);

            if (page == SDRN_I2C_GATE_PAGE && reg == SDRN_I2C_GATE_REG && length >= 1u) {
                m->tuner_gate_open = (data[0] & 0x08u) != 0u;
                if (m->tuner_gate_open) {
                    ++m->tuner_gate_open_count;
                } else {
                    ++m->tuner_gate_close_count;
                }
            }
        }
        *transferred = length;
        return SDRN_OK;
    }

    if (index == SDRN_CMD_I2C_RD || index == SDRN_CMD_I2C_WR) {
        uint8_t addr = (uint8_t)((value & 0xFFu) >> 1);
        uint8_t reg = (uint8_t)(value >> 8);
        if (addr != SDRN_R820T_ADDR) return SDRN_ERR_NOT_FOUND;
        if (!m->tuner_gate_open) return SDRN_ERR_STATE;

        if (!input) {
            m->compact_i2c_write_count++;
        }

        if (input) {
            if (reg == 0x00u) {
                static const uint8_t status[5] = {0x69,0x81,0x6B,0xFF,0xE7};
                for (uint16_t i = 0; i < length; ++i) data[i] = status[i < 5 ? i : 4];
            } else {
                mem_read(data, &m->tuner[reg], length);
            }
        } else {
            for (uint16_t i = 0; i < length; ++i) {
                m->tuner_reg_write_count[(uint8_t)(reg + i)]++;
            }
            mem_write(&m->tuner[reg], data, length);
        }
        *transferred = length;
        return SDRN_OK;
    }

    if (index == SDRN_CMD_I2C_DA_RD || index == SDRN_CMD_I2C_DA_WR) {
        uint8_t addr = (uint8_t)((value & 0xFFu) >> 1);
        if (addr != SDRN_R820T_ADDR) return SDRN_ERR_NOT_FOUND;
        if (!m->tuner_gate_open) return SDRN_ERR_STATE;
        if (input) {
            m->direct_i2c_read_count++;
            if (m->direct_i2c_pointer == 0x00u) {
                static const uint8_t status[5] = {0x69,0x81,0x6B,0xFF,0xE7};
                for (uint16_t i = 0; i < length; ++i) data[i] = status[i < 5 ? i : 4];
            } else {
                mem_read(data, &m->tuner[m->direct_i2c_pointer], length);
            }
        } else {
            if (length < 1u) return SDRN_ERR_IO;
            m->direct_i2c_pointer = data[0];
            m->direct_i2c_write_count++;
            if (length > 1u) {
                for (uint16_t i = 1u; i < length; ++i) {
                    m->tuner_reg_write_count[(uint8_t)(m->direct_i2c_pointer + i - 1u)]++;
                }
                mem_write(&m->tuner[m->direct_i2c_pointer], &data[1], (size_t)length - 1u);
            }
        }
        *transferred = length;
        return SDRN_OK;
    }

    if (value + length > sizeof(m->regs)) return SDRN_ERR_IO;
    if (input) {
        mem_read(data, &m->regs[value], length);
    } else {
        mem_write(&m->regs[value], data, length);
        if (value == SDRN_USB_EPA_CTL && length >= 2u) {
            if (data[0] == 0x10u && data[1] == 0x02u) {
                m->endpoint_reset_armed = true;
                if (m->stream_endpoint_active) {
                    m->stream_endpoint_active = false;
                    m->endpoint_quiesce_count++;
                }
            } else if (data[0] == 0x00u && data[1] == 0x00u &&
                       m->endpoint_reset_armed) {
                m->stale_iq_fifo = false;
                m->endpoint_reset_armed = false;
                m->halt_needs_clear = true;
            }
        }
    }
    *transferred = length;
    return SDRN_OK;
}

static sdrn_status bulk_read(
    void *opaque, void *handle, uint8_t endpoint,
    uint8_t *data, int length, uint32_t timeout_ms,
    int *transferred, char *error, size_t error_cap
) {
    (void)handle; (void)timeout_ms; (void)error; (void)error_cap;
    mock_ctx *m = (mock_ctx *)opaque;
    if (endpoint != SDRN_TS_ENDPOINT) return SDRN_ERR_NOT_FOUND;
    if (m->halt_needs_clear) {
        if (error && error_cap) snprintf(error, error_cap, "mock USB halt not cleared");
        *transferred = 0;
        return SDRN_ERR_USB;
    }
    if (m->sync_zero_timeout_enabled) {
        /* armed bulk request can time out with zero bytes */
        /* endpoint still needs reset before control traffic */
        m->stream_endpoint_active = true;
        *transferred = 0;
        return SDRN_ERR_TIMEOUT;
    }

    /* page 0 reg 0x19 value 0x05 marks raw iq mode */
    /* endpoint A reset state reads as PIPE */
    if (m->demod[0][0x19] == 0x05u) {
        if (m->regs[SDRN_USB_EPA_CTL] == 0x10u &&
            m->regs[SDRN_USB_EPA_CTL + 1u] == 0x02u) {
            if (error && error_cap) {
                snprintf(error, error_cap, "mock endpoint A held in reset (PIPE)");
            }
            *transferred = 0;
            return SDRN_ERR_USB;
        }
        for (int i = 0; i < length; ++i) {
            data[i] = (uint8_t)(96 + ((i * 37 + m->continuity) & 0x3F));
        }
        m->continuity++;
        m->stale_iq_fifo = true;
        m->stream_endpoint_active = length > 0;
        *transferred = length;
        return SDRN_OK;
    }

    /* stale iq data survives mode change without full endpoint reset */
    if (m->stale_iq_fifo) {
        for (int i = 0; i < length; ++i) {
            data[i] = (uint8_t)(80 + ((i * 29 + m->continuity) & 0x5F));
        }
        m->stale_iq_fifo = false;
        m->stream_endpoint_active = length > 0;
        *transferred = length;
        return SDRN_OK;
    }

    int packets = length / 188;
    if (timeout_ms == 123u && packets > 1) packets = 1;
    if (packets > 32) packets = 32;
    int n = packets * 188;
    for (int p = 0; p < packets; ++p) {
        uint8_t *pkt = data + p * 188;
        memset(pkt, 0xFF, 188);
        pkt[0] = 0x47;
        pkt[1] = 0x00;
        pkt[2] = 0x00;
        pkt[3] = (uint8_t)(0x10u | (m->continuity++ & 0x0Fu));
    }
    *transferred = n;
    if (n > 0) m->stream_endpoint_active = true;
    if (timeout_ms == 123u) return SDRN_ERR_TIMEOUT;
    return SDRN_OK;
}

static sdrn_status clear_halt(
    void *opaque, void *handle, uint8_t endpoint,
    char *error, size_t error_cap
) {
    (void)handle; (void)error; (void)error_cap;
    mock_ctx *m = (mock_ctx *)opaque;
    if (endpoint != SDRN_TS_ENDPOINT) return SDRN_ERR_NOT_FOUND;
    m->clear_halt_calls++;
    if (m->clear_halt_status != SDRN_OK) {
        m->last_native_error = m->clear_halt_native_code;
        if (error && error_cap) {
            snprintf(error, error_cap, "mock clear-halt failure");
        }
        return m->clear_halt_status;
    }
    m->halt_needs_clear = false;
    return SDRN_OK;
}

static sdrn_status recover_session(
    void *opaque, void **handle, int interface_number,
    const sdrn_device_info *expected_info, char *error, size_t error_cap
) {
    (void)interface_number; (void)expected_info; (void)error; (void)error_cap;
    mock_ctx *m = (mock_ctx *)opaque;
    if (!m || !handle || !*handle) return SDRN_ERR_INVALID_ARGUMENT;
    if (!m->recoverable_pipe_enabled || !m->recoverable_pipe_pending) {
        return SDRN_ERR_UNSUPPORTED;
    }
    m->recover_session_calls++;
    if (!m->recoverable_pipe_retry_sticky) m->recoverable_pipe_pending = false;
    *handle = m;
    return SDRN_OK;
}

static sdrn_status endpoint_info(
    void *opaque, void *handle, int interface_number, uint8_t endpoint,
    uint16_t *max_packet_size, bool *is_bulk,
    char *error, size_t error_cap
) {
    (void)opaque; (void)handle; (void)error; (void)error_cap;
    if (interface_number != 0 || endpoint != SDRN_TS_ENDPOINT) return SDRN_ERR_NOT_FOUND;
    *max_packet_size = 512;
    *is_bulk = true;
    return SDRN_OK;
}


typedef struct mock_async_state {
    mock_ctx *mock;
    uint8_t endpoint;
    uint32_t transfer_size;
    uint32_t transfer_timeout_ms;
    sdrn_usb_async_completion_fn callback;
    void *callback_user;
    sdrn_mutex *mutex;
    sdrn_thread *thread;
    bool stopping;
} mock_async_state;

static int mock_async_thread(void *opaque) {
    mock_async_state *state = (mock_async_state *)opaque;
    uint8_t *buffer = (uint8_t *)malloc(state->transfer_size);
    if (!buffer) {
        state->callback(
            state->callback_user, SDRN_ERR_NO_MEMORY, 0, NULL, 0u,
            "mock async transfer buffer allocation failed"
        );
        return 0;
    }

    unsigned successes = 0u;
    for (;;) {
        sdrn_mutex_lock(state->mutex);
        bool stopping = state->stopping;
        sdrn_mutex_unlock(state->mutex);
        if (stopping) break;

        if (mock_async_silence_get(state->mock)) {
            /* async reads may stay pending until cancellation */
            sdrn_sleep_ms(5u);
            continue;
        }

        if (state->mock->async_fault_enabled &&
            successes >= state->mock->async_fault_after_successes) {
            if (state->mock->async_fault_delay_ms > 0u) {
                sdrn_sleep_ms(state->mock->async_fault_delay_ms);
            }
            sdrn_mutex_lock(state->mutex);
            stopping = state->stopping;
            sdrn_mutex_unlock(state->mutex);
            if (stopping) break;

            state->mock->last_native_error = state->mock->async_fault_native_code;
            state->callback(
                state->callback_user,
                state->mock->async_fault_status,
                state->mock->async_fault_native_code,
                NULL,
                0u,
                state->mock->async_fault_detail[0]
                    ? state->mock->async_fault_detail
                    : "mock async terminal USB failure"
            );
            break;
        }

        int transferred = 0;
        char error[128] = {0};
        sdrn_status st = bulk_read(
            state->mock,
            state->mock,
            state->endpoint,
            buffer,
            (int)state->transfer_size,
            state->transfer_timeout_ms,
            &transferred,
            error,
            sizeof(error)
        );
        state->callback(
            state->callback_user,
            st,
            0,
            transferred > 0 ? buffer : NULL,
            transferred > 0 ? (size_t)transferred : 0u,
            error[0] ? error : NULL
        );
        if (st != SDRN_OK && st != SDRN_ERR_TIMEOUT) break;
        if (st == SDRN_OK) ++successes;
        sdrn_sleep_ms(1u);
    }

    free(buffer);
    return 0;
}

static sdrn_status async_bulk_start(
    void *opaque,
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
    (void)handle;
    (void)transfer_count;
    (void)error;
    (void)error_cap;
    if (!opaque || !callback || !out_async_handle || transfer_size_bytes == 0u) {
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    *out_async_handle = NULL;
    mock_async_state *state = (mock_async_state *)calloc(1, sizeof(*state));
    if (!state) return SDRN_ERR_NO_MEMORY;
    state->mock = (mock_ctx *)opaque;
    state->endpoint = endpoint;
    state->transfer_size = transfer_size_bytes;
    state->transfer_timeout_ms = transfer_timeout_ms;
    state->callback = callback;
    state->callback_user = callback_user;
    if (mock_async_silence_get(state->mock)) {
        state->mock->stream_endpoint_active = true;
    }
    sdrn_status st = sdrn_mutex_create(&state->mutex);
    if (st != SDRN_OK) {
        free(state);
        return st;
    }
    st = sdrn_thread_create(&state->thread, mock_async_thread, state);
    if (st != SDRN_OK) {
        sdrn_mutex_destroy(state->mutex);
        free(state);
        return st;
    }
    *out_async_handle = state;
    return SDRN_OK;
}

static sdrn_status async_bulk_stop(
    void *opaque,
    void *async_handle,
    char *error,
    size_t error_cap
) {
    (void)opaque;
    (void)error;
    (void)error_cap;
    mock_async_state *state = (mock_async_state *)async_handle;
    if (!state) return SDRN_OK;
    sdrn_mutex_lock(state->mutex);
    state->stopping = true;
    sdrn_mutex_unlock(state->mutex);
    sdrn_thread_join_destroy(state->thread);
    sdrn_mutex_destroy(state->mutex);
    mock_ctx *mock = state->mock;
    free(state);

    if (mock && mock->async_stop_status != SDRN_OK) {
        mock->last_native_error = mock->async_stop_native_code;
        if (error && error_cap) {
            snprintf(error, error_cap, "mock async backend stop failure");
        }
        return mock->async_stop_status;
    }
    return SDRN_OK;
}

static int32_t native_error_code(void *opaque) {
    if (!opaque) return 0;
    return ((mock_ctx *)opaque)->last_native_error;
}

static const sdrn_usb_ops OPS = {
    destroy, list, open_dev, close_dev, claim, release,
    control, bulk_read, clear_halt, recover_session, endpoint_info,
    async_bulk_start, async_bulk_stop, native_error_code
};

sdrn_status sdrn_test_mock_backend_create(
    const sdrn_usb_ops **out_ops,
    void **out_ctx
) {
    if (!out_ops || !out_ctx) return SDRN_ERR_INVALID_ARGUMENT;
    mock_ctx *m = (mock_ctx *)calloc(1, sizeof(*m));
    if (!m) return SDRN_ERR_NO_MEMORY;
    sdrn_status mutex_st = sdrn_mutex_create(&m->async_silence_mutex);
    if (mutex_st != SDRN_OK) {
        free(m);
        return mutex_st;
    }
    m->async_silence_enabled = false;

    /* idle demod state */
    m->regs[SDRN_USB_SYSCTL_0] = 0x01;
    m->regs[SDRN_SYS_GPIO_OUT_VAL] = 0x18;
    m->regs[SDRN_SYS_GPIO_DIR] = 0x06;
    m->regs[SDRN_SYS_GPIO_OUT_EN] = 0x19;
    m->regs[SDRN_SYS_DEMOD_CTL1] = 0x02;
    m->regs[SDRN_SYS_DEMOD_CTL] = 0x20;

    /* FSM 11 is full dvb lock */
    m->demod[3][0x51] = (uint8_t)(11u << 3);
    m->demod[3][0x05] = 0x10;
    m->demod[3][0x3C] = 0x00;
    m->demod[4][0x0C] = 0x01;
    m->demod[4][0x0D] = 0x00;
    m->demod[3][0x4E] = 0x00;
    m->demod[3][0x4F] = 0x03;

    *out_ops = &OPS;
    *out_ctx = m;
    return SDRN_OK;
}

unsigned sdrn_test_mock_compact_i2c_write_count(void *opaque) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->compact_i2c_write_count;
}


unsigned sdrn_test_mock_direct_i2c_write_count(void *opaque) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->direct_i2c_write_count;
}

unsigned sdrn_test_mock_direct_i2c_read_count(void *opaque) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->direct_i2c_read_count;
}





unsigned sdrn_test_mock_tuner_gate_open_count(void *opaque) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->tuner_gate_open_count;
}

unsigned sdrn_test_mock_tuner_gate_close_count(void *opaque) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->tuner_gate_close_count;
}

unsigned sdrn_test_mock_tuner_reg_write_count(void *opaque, uint8_t reg) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->tuner_reg_write_count[reg];
}


void sdrn_test_mock_recovery_configure(void *opaque, bool retry_sticky) {
    if (!opaque) return;
    mock_ctx *m = (mock_ctx *)opaque;
    m->recoverable_pipe_enabled = true;
    m->recoverable_pipe_pending = false;
    m->recoverable_pipe_injected = false;
    m->recoverable_pipe_retry_sticky = retry_sticky;
}

void sdrn_test_mock_require_stream_quiesce(void *opaque, bool enabled) {
    if (!opaque) return;
    ((mock_ctx *)opaque)->require_endpoint_quiesce_before_control = enabled;
}

unsigned sdrn_test_mock_recover_session_calls(void *opaque) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->recover_session_calls;
}

void sdrn_test_mock_async_fault_configure(
    void *opaque,
    sdrn_status status,
    int32_t native_code,
    unsigned after_successes,
    uint32_t delay_ms,
    const char *detail
) {
    if (!opaque) return;
    mock_ctx *m = (mock_ctx *)opaque;
    m->async_fault_enabled = true;
    m->async_fault_status = status;
    m->async_fault_native_code = native_code;
    m->async_fault_after_successes = after_successes;
    m->async_fault_delay_ms = delay_ms;
    snprintf(
        m->async_fault_detail, sizeof(m->async_fault_detail), "%s",
        detail ? detail : "mock async terminal USB failure"
    );
}

void sdrn_test_mock_async_fault_clear(void *opaque) {
    if (!opaque) return;
    mock_ctx *m = (mock_ctx *)opaque;
    m->async_fault_enabled = false;
    m->async_fault_status = SDRN_OK;
    m->async_fault_native_code = 0;
    m->async_fault_after_successes = 0u;
    m->async_fault_delay_ms = 0u;
    m->async_fault_detail[0] = '\0';
}

void sdrn_test_mock_async_stop_failure_configure(
    void *opaque, sdrn_status status, int32_t native_code
) {
    if (!opaque) return;
    mock_ctx *m = (mock_ctx *)opaque;
    m->async_stop_status = status;
    m->async_stop_native_code = native_code;
}

void sdrn_test_mock_clear_halt_failure_configure(
    void *opaque, sdrn_status status, int32_t native_code
) {
    if (!opaque) return;
    mock_ctx *m = (mock_ctx *)opaque;
    m->clear_halt_status = status;
    m->clear_halt_native_code = native_code;
}

unsigned sdrn_test_mock_endpoint_quiesce_count(void *opaque) {
    if (!opaque) return 0u;
    return ((mock_ctx *)opaque)->endpoint_quiesce_count;
}

void sdrn_test_mock_async_silence_configure(void *opaque, bool enabled) {
    if (!opaque) return;
    mock_async_silence_set((mock_ctx *)opaque, enabled);
}

void sdrn_test_mock_sync_zero_timeout_configure(void *opaque, bool enabled) {
    if (!opaque) return;
    ((mock_ctx *)opaque)->sync_zero_timeout_enabled = enabled;
}