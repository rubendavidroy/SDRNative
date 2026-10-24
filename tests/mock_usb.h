/*
 * SDRNative
 * Mock USB backend interface
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef SDRNATIVE_TEST_MOCK_USB_H
#define SDRNATIVE_TEST_MOCK_USB_H

#include "internal.h"

sdrn_status sdrn_test_mock_backend_create(
    const sdrn_usb_ops **out_ops,
    void **out_ctx
);

unsigned sdrn_test_mock_compact_i2c_write_count(void *opaque);
unsigned sdrn_test_mock_direct_i2c_write_count(void *opaque);
unsigned sdrn_test_mock_direct_i2c_read_count(void *opaque);
unsigned sdrn_test_mock_recover_session_calls(void *opaque);
void sdrn_test_mock_recovery_configure(void *opaque, bool retry_sticky);
void sdrn_test_mock_require_stream_quiesce(void *opaque, bool enabled);
unsigned sdrn_test_mock_tuner_gate_open_count(void *opaque);
unsigned sdrn_test_mock_tuner_gate_close_count(void *opaque);
unsigned sdrn_test_mock_tuner_reg_write_count(void *opaque, uint8_t reg);
void sdrn_test_mock_async_fault_configure(
    void *opaque, sdrn_status status, int32_t native_code,
    unsigned after_successes, uint32_t delay_ms, const char *detail
);
void sdrn_test_mock_async_fault_clear(void *opaque);
void sdrn_test_mock_async_stop_failure_configure(
    void *opaque, sdrn_status status, int32_t native_code
);
void sdrn_test_mock_clear_halt_failure_configure(
    void *opaque, sdrn_status status, int32_t native_code
);
void sdrn_test_mock_async_silence_configure(void *opaque, bool enabled);
void sdrn_test_mock_sync_zero_timeout_configure(void *opaque, bool enabled);
unsigned sdrn_test_mock_endpoint_quiesce_count(void *opaque);

#endif