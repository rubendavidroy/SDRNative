/*
 * SDRNative
 * Asynchronous fault tests
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "sdrnative.h"
#include "internal.h"
#include "mock_usb.h"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct test_device {
    sdrn_device *dev;
    void *mock;
} test_device;

static test_device open_mock(void) {
    const sdrn_usb_ops *ops = NULL;
    void *ctx = NULL;
    assert(sdrn_test_mock_backend_create(&ops, &ctx) == SDRN_OK);
    sdrn_device *dev = NULL;
    assert(sdrn_device_create_with_backend(ops, ctx, 0u, &dev) == SDRN_OK);
    test_device out = {dev, ctx};
    return out;
}

static void start_iq_async(sdrn_device *dev) {
    sdrn_iq_tune_info iq;
    assert(sdrn_iq_start(dev, 100700000ull, 2400000u, 0u, &iq) == SDRN_OK);

    sdrn_async_config cfg;
    sdrn_async_config_init(&cfg);
    cfg.transfer_size_bytes = 4096u;
    cfg.transfer_count = 4u;
    cfg.queue_depth = 8u;
    cfg.transfer_timeout_ms = 0u;
    cfg.overflow_policy = SDRN_ASYNC_DROP_OLDEST;
    assert(sdrn_async_start(dev, &cfg) == SDRN_OK);
}

static sdrn_status read_until_terminal(sdrn_device *dev, unsigned max_reads) {
    unsigned char buffer[4096];
    for (unsigned i = 0u; i < max_reads; ++i) {
        size_t got = 0u;
        uint64_t sequence = 0u;
        sdrn_status st = sdrn_async_read(
            dev, buffer, sizeof(buffer), &got, &sequence, 1000u
        );
        if (st != SDRN_OK && st != SDRN_ERR_TIMEOUT) return st;
    }
    return SDRN_OK;
}

static void assert_terminal_error(
    sdrn_device *dev,
    sdrn_status expected,
    int expect_device_gone,
    const char *message_fragment
) {
    sdrn_error_info info;
    assert(sdrn_get_last_error_info(dev, &info) == SDRN_OK);
    assert(info.status == (int32_t)expected);
    assert(info.domain == (int32_t)SDRN_ERROR_DOMAIN_STREAM);
    assert((info.flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0u);
    if (expect_device_gone) {
        assert((info.flags & SDRN_ERROR_FLAG_DEVICE_GONE) != 0u);
    } else {
        assert((info.flags & SDRN_ERROR_FLAG_DEVICE_GONE) == 0u);
    }
    if (message_fragment) {
        assert(strstr(info.message, message_fragment) != NULL);
    }
}

static void test_blocked_read_wakes_on_no_device(void) {
    test_device t = open_mock();
    sdrn_test_mock_async_fault_configure(
        t.mock, SDRN_ERR_NOT_FOUND, -4, 0u, 30u,
        "mock device disconnected while async read was pending"
    );
    start_iq_async(t.dev);

    unsigned char buffer[4096];
    size_t got = 0u;
    uint64_t sequence = 0u;
    const uint64_t started = sdrn_now_ms();
    sdrn_status st = sdrn_async_read(
        t.dev, buffer, sizeof(buffer), &got, &sequence, UINT32_MAX
    );
    const uint64_t elapsed = sdrn_now_ms() - started;

    assert(st == SDRN_ERR_NOT_FOUND);
    assert(got == 0u);
    assert(elapsed >= 10u);
    assert(elapsed < 2000u);
    assert(sdrn_async_is_running(t.dev) == 0);
    assert_terminal_error(t.dev, SDRN_ERR_NOT_FOUND, 1, "device disconnected");

    sdrn_async_stats stats;
    assert(sdrn_async_get_stats(t.dev, &stats) == SDRN_OK);
    assert(stats.transfer_errors == 1u);
    assert(stats.terminal_status == (int32_t)SDRN_ERR_NOT_FOUND);
    assert(stats.terminal_native_code == -4);

    /* NO_DEVICE releases software state without hardware restore */
    assert(sdrn_stop(t.dev) == SDRN_ERR_NOT_FOUND);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
    assert(sdrn_test_mock_endpoint_quiesce_count(t.mock) == 0u);
    assert_terminal_error(t.dev, SDRN_ERR_NOT_FOUND, 1, "device disconnected");

    sdrn_close(t.dev);
}

static void test_stall_terminal_drains_queue_then_recovers_handle(void) {
    test_device t = open_mock();
    sdrn_test_mock_async_fault_configure(
        t.mock, SDRN_ERR_USB, -9, 1u, 1u,
        "mock libusb asynchronous bulk STALL"
    );
    start_iq_async(t.dev);

    /* queued producer data stays valid before terminal fault is surfaced */
    sdrn_status terminal = read_until_terminal(t.dev, 8u);
    assert(terminal == SDRN_ERR_USB);
    assert_terminal_error(t.dev, SDRN_ERR_USB, 0, "STALL");

    sdrn_async_stats stats;
    assert(sdrn_async_get_stats(t.dev, &stats) == SDRN_OK);
    assert(stats.transfers_completed >= 1u);
    assert(stats.transfer_errors == 1u);
    assert(stats.terminal_status == (int32_t)SDRN_ERR_USB);
    assert(stats.terminal_native_code == -9);

    /* terminal stream error stays primary through cleanup */
    assert(sdrn_stop(t.dev) == SDRN_ERR_USB);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
    assert(sdrn_test_mock_endpoint_quiesce_count(t.mock) >= 1u);
    assert_terminal_error(t.dev, SDRN_ERR_USB, 0, "STALL");

    /* stop stays idempotent after native ownership is released */
    assert(sdrn_stop(t.dev) == SDRN_OK);
    sdrn_error_info cleared;
    assert(sdrn_get_last_error_info(t.dev, &cleared) == SDRN_OK);
    assert(cleared.status == (int32_t)SDRN_OK);
    assert(cleared.domain == (int32_t)SDRN_ERROR_DOMAIN_NONE);

    /* non disconnect terminal fault must not poison handle */
    /* new native mode can start after cleanup */
    sdrn_test_mock_async_fault_clear(t.mock);
    sdrn_iq_tune_info iq;
    assert(sdrn_iq_start(t.dev, 101100000ull, 2048000u, 0u, &iq) == SDRN_OK);
    assert(sdrn_stop(t.dev) == SDRN_OK);
    sdrn_close(t.dev);
}

static void test_terminal_error_beats_cleanup_error(void) {
    test_device t = open_mock();
    sdrn_test_mock_async_fault_configure(
        t.mock, SDRN_ERR_USB, -9, 1u, 1u,
        "mock resubmit failure after completed producer buffer"
    );
    sdrn_test_mock_async_stop_failure_configure(t.mock, SDRN_ERR_IO, -99);
    start_iq_async(t.dev);

    assert(read_until_terminal(t.dev, 8u) == SDRN_ERR_USB);
    assert(sdrn_stop(t.dev) == SDRN_ERR_USB);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);

    sdrn_error_info info;
    assert(sdrn_get_last_error_info(t.dev, &info) == SDRN_OK);
    assert(info.status == (int32_t)SDRN_ERR_USB);
    assert(info.native_code == -9);
    assert((info.flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0u);
    assert(strstr(info.message, "resubmit failure") != NULL);

    sdrn_close(t.dev);
}


static void test_public_async_stop_preserves_terminal_over_cleanup(void) {
    test_device t = open_mock();
    sdrn_test_mock_async_fault_configure(
        t.mock, SDRN_ERR_USB, -9, 1u, 1u,
        "mock terminal fault before public async stop"
    );
    sdrn_test_mock_async_stop_failure_configure(t.mock, SDRN_ERR_IO, -88);
    start_iq_async(t.dev);

    assert(read_until_terminal(t.dev, 8u) == SDRN_ERR_USB);
    assert(sdrn_async_stop(t.dev) == SDRN_ERR_USB);
    assert(sdrn_async_is_running(t.dev) == 0);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IQ);
    assert_terminal_error(t.dev, SDRN_ERR_USB, 0, "terminal fault");

    /* async only stop leaves native mode owned by handle */
    /* hardware stop preserves terminal diagnosis */
    sdrn_test_mock_async_stop_failure_configure(t.mock, SDRN_OK, 0);
    assert(sdrn_stop(t.dev) == SDRN_ERR_USB);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
    assert_terminal_error(t.dev, SDRN_ERR_USB, 0, "terminal fault");
    sdrn_close(t.dev);
}

static void test_backend_stop_failure_still_releases_mode(void) {
    test_device t = open_mock();
    start_iq_async(t.dev);

    /* stream must cross endpoint before stop cleanup error is injected */
    /* async_bulk_stop returns only after callbacks are drained */
    unsigned char buffer[4096];
    size_t got = 0u;
    uint64_t sequence = 0u;
    assert(sdrn_async_read(
        t.dev, buffer, sizeof(buffer), &got, &sequence, 1000u
    ) == SDRN_OK);
    assert(got > 0u);

    sdrn_test_mock_async_stop_failure_configure(t.mock, SDRN_ERR_IO, -77);
    assert(sdrn_stop(t.dev) == SDRN_ERR_IO);
    assert(sdrn_async_is_running(t.dev) == 0);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
    assert(sdrn_test_mock_endpoint_quiesce_count(t.mock) >= 1u);

    sdrn_error_info info;
    assert(sdrn_get_last_error_info(t.dev, &info) == SDRN_OK);
    assert(info.status == (int32_t)SDRN_ERR_IO);
    assert(info.native_code == -77);
    assert(strstr(info.message, "async stream stop failed") != NULL);

    sdrn_close(t.dev);
}


static void test_disconnect_reported_by_backend_stop_skips_restore(void) {
    test_device t = open_mock();
    start_iq_async(t.dev);

    /* device can disappear while cancellation is in flight */
    /* backend still joins worker before reporting NO_DEVICE */
    sdrn_test_mock_async_stop_failure_configure(t.mock, SDRN_ERR_NOT_FOUND, -4);
    assert(sdrn_stop(t.dev) == SDRN_ERR_NOT_FOUND);
    assert(sdrn_async_is_running(t.dev) == 0);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
    assert(sdrn_test_mock_endpoint_quiesce_count(t.mock) == 0u);

    sdrn_error_info info;
    assert(sdrn_get_last_error_info(t.dev, &info) == SDRN_OK);
    assert(info.status == (int32_t)SDRN_ERR_NOT_FOUND);
    assert(info.domain == (int32_t)SDRN_ERROR_DOMAIN_STREAM);
    assert(info.native_code == -4);
    assert((info.flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0u);
    assert((info.flags & SDRN_ERROR_FLAG_DEVICE_GONE) != 0u);
    assert(strstr(info.message, "async stream stop failed") != NULL);

    /* dead handle stays DEVICE_GONE until close and reopen */
    assert(sdrn_stop(t.dev) == SDRN_ERR_NOT_FOUND);
    sdrn_close(t.dev);
}


static void test_disconnect_confirmed_during_backend_stop(void) {
    test_device t = open_mock();
    sdrn_test_mock_async_fault_configure(
        t.mock, SDRN_ERR_USB, -9, 1u, 1u,
        "mock bulk STALL/PIPE before disconnect"
    );
    sdrn_test_mock_async_stop_failure_configure(t.mock, SDRN_ERR_NOT_FOUND, -4);
    start_iq_async(t.dev);

    /* STALL or PIPE can arrive before device disappearance */
    /* NO_DEVICE marks disconnect as final */
    assert(read_until_terminal(t.dev, 8u) == SDRN_ERR_USB);
    assert_terminal_error(t.dev, SDRN_ERR_USB, 0, "STALL/PIPE");

    assert(sdrn_stop(t.dev) == SDRN_ERR_NOT_FOUND);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
    assert(sdrn_test_mock_endpoint_quiesce_count(t.mock) == 0u);

    sdrn_error_info info;
    assert(sdrn_get_last_error_info(t.dev, &info) == SDRN_OK);
    assert(info.status == (int32_t)SDRN_ERR_NOT_FOUND);
    assert(info.domain == (int32_t)SDRN_ERROR_DOMAIN_STREAM);
    assert(info.native_code == -4);
    assert((info.flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0u);
    assert((info.flags & SDRN_ERROR_FLAG_DEVICE_GONE) != 0u);
    assert(strstr(info.message, "stream fault") != NULL);
    assert(strstr(info.message, "STALL/PIPE") != NULL);

    sdrn_close(t.dev);
}

static void test_disconnect_confirmed_during_quiesce(void) {
    test_device t = open_mock();
    sdrn_test_mock_async_fault_configure(
        t.mock, SDRN_ERR_USB, -9, 1u, 1u,
        "mock bulk STALL/PIPE before disconnect"
    );
    start_iq_async(t.dev);

    assert(read_until_terminal(t.dev, 8u) == SDRN_ERR_USB);
    assert_terminal_error(t.dev, SDRN_ERR_USB, 0, "STALL/PIPE");

    /* async completion can report PIPE before NO_DEVICE */
    sdrn_test_mock_clear_halt_failure_configure(t.mock, SDRN_ERR_NOT_FOUND, -4);

    assert(sdrn_stop(t.dev) == SDRN_ERR_NOT_FOUND);
    assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
    assert(sdrn_test_mock_endpoint_quiesce_count(t.mock) == 0u);

    sdrn_error_info info;
    assert(sdrn_get_last_error_info(t.dev, &info) == SDRN_OK);
    assert(info.status == (int32_t)SDRN_ERR_NOT_FOUND);
    assert(info.domain == (int32_t)SDRN_ERROR_DOMAIN_STREAM);
    assert(info.native_code == -4);
    assert((info.flags & SDRN_ERROR_FLAG_TERMINAL_STREAM) != 0u);
    assert((info.flags & SDRN_ERROR_FLAG_DEVICE_GONE) != 0u);
    assert(strstr(info.message, "stream fault") != NULL);
    assert(strstr(info.message, "STALL/PIPE") != NULL);

    sdrn_close(t.dev);
}

static void test_cancel_vs_terminal_race(void) {
    for (unsigned i = 0u; i < 50u; ++i) {
        test_device t = open_mock();
        sdrn_test_mock_async_fault_configure(
            t.mock, SDRN_ERR_USB, -1, 0u, 1u,
            "mock async race fault"
        );
        start_iq_async(t.dev);

        /* cancellation and terminal callback may race */
        /* either winner must leave async and native state releasable */
        sdrn_status async_st = sdrn_async_stop(t.dev);
        assert(async_st == SDRN_OK || async_st == SDRN_ERR_USB);
        assert(sdrn_async_is_running(t.dev) == 0);

        sdrn_status stop_st = sdrn_stop(t.dev);
        if (async_st == SDRN_ERR_USB) {
            assert(stop_st == SDRN_ERR_USB);
        } else {
            assert(stop_st == SDRN_OK);
        }
        assert(sdrn_get_state(t.dev) == SDRN_STATE_IDLE);
        sdrn_close(t.dev);
    }
}

int main(void) {
    test_blocked_read_wakes_on_no_device();
    test_stall_terminal_drains_queue_then_recovers_handle();
    test_terminal_error_beats_cleanup_error();
    test_public_async_stop_preserves_terminal_over_cleanup();
    test_backend_stop_failure_still_releases_mode();
    test_disconnect_reported_by_backend_stop_skips_restore();
    test_disconnect_confirmed_during_backend_stop();
    test_disconnect_confirmed_during_quiesce();
    test_cancel_vs_terminal_race();
    puts("async errors: pass");
    return 0;
}