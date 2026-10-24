/*
 * SDRNative
 * Asynchronous streaming tests
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


static void wait_for_overflow(
    sdrn_device *dev,
    sdrn_async_overflow_policy policy,
    uint32_t timeout_ms,
    sdrn_async_stats *out_stats
) {
    const uint64_t deadline = sdrn_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        assert(sdrn_async_get_stats(dev, out_stats) == SDRN_OK);
        const uint64_t drops = policy == SDRN_ASYNC_DROP_OLDEST
            ? out_stats->buffers_dropped_oldest
            : out_stats->buffers_dropped_newest;
        if (drops > 0u) return;
        if (sdrn_now_ms() >= deadline) break;
        sdrn_sleep_ms(1u);
    }

    fprintf(
        stderr,
        "async overflow timed out: policy=%d callbacks=%llu completed=%llu "
        "enqueued=%llu q=%u high=%u drop_old=%llu drop_new=%llu\n",
        (int)policy,
        (unsigned long long)out_stats->transfer_callbacks,
        (unsigned long long)out_stats->transfers_completed,
        (unsigned long long)out_stats->buffers_enqueued,
        out_stats->queue_current,
        out_stats->queue_high_water,
        (unsigned long long)out_stats->buffers_dropped_oldest,
        (unsigned long long)out_stats->buffers_dropped_newest
    );
    assert(0 && "async producer did not reach the requested overflow policy before timeout");
}

static int read_until_sequence_gap(
    sdrn_device *dev,
    uint64_t initial_sequence,
    uint64_t *out_before,
    uint64_t *out_after
) {
    unsigned char buffer[4096];
    uint64_t previous = initial_sequence;

    for (unsigned attempt = 0u; attempt < 32u; ++attempt) {
        size_t got = 0u;
        uint64_t sequence = UINT64_MAX;
        sdrn_status st = sdrn_async_read(
            dev, buffer, sizeof(buffer), &got, &sequence, 1000u
        );
        if (st == SDRN_ERR_TIMEOUT) continue;
        assert(st == SDRN_OK);
        assert(got > 0u);
        assert(sequence >= previous);

        if (sequence > previous + 1u) {
            if (out_before) *out_before = previous;
            if (out_after) *out_after = sequence;
            return 1;
        }
        if (sequence > previous) previous = sequence;
    }
    return 0;
}

static sdrn_device *open_mock(void) {
    const sdrn_usb_ops *ops = NULL;
    void *ctx = NULL;
    assert(sdrn_test_mock_backend_create(&ops, &ctx) == SDRN_OK);
    sdrn_device *dev = NULL;
    assert(sdrn_device_create_with_backend(ops, ctx, 0u, &dev) == SDRN_OK);
    return dev;
}

static void test_iq_async(void) {
    sdrn_device *dev = open_mock();
    sdrn_iq_tune_info iq;
    assert(sdrn_iq_start(dev, 100700000ull, 2400000u, 0u, &iq) == SDRN_OK);

    sdrn_async_config cfg;
    sdrn_async_config_init(&cfg);
    assert(cfg.struct_size == sizeof(cfg));
    assert(cfg.config_version == SDRN_ASYNC_CONFIG_VERSION);
    cfg.transfer_size_bytes = 4096u;
    cfg.transfer_count = 4u;
    cfg.queue_depth = 4u;
    cfg.overflow_policy = SDRN_ASYNC_DROP_OLDEST;

    assert(sdrn_async_start(dev, &cfg) == SDRN_OK);
    assert(sdrn_async_is_running(dev) == 1);

    unsigned char sync_buf[64];
    size_t sync_got = 0u;
    assert(sdrn_stream_read(dev, sync_buf, sizeof(sync_buf), &sync_got, 10u) == SDRN_ERR_STATE);
    assert(sdrn_iq_tune(dev, 101100000ull, &iq) == SDRN_ERR_STATE);

    unsigned char buf[4096];
    size_t got = 0u;
    uint64_t sequence = UINT64_MAX;
    assert(sdrn_async_read(dev, buf, sizeof(buf), &got, &sequence, 1000u) == SDRN_OK);
    assert(got > 0u);
    assert(buf[0] != buf[1]);

    /* wait for observed queue overflow instead of host timing */
    sdrn_async_stats stats;
    wait_for_overflow(dev, SDRN_ASYNC_DROP_OLDEST, 2000u, &stats);
    assert(stats.struct_size == sizeof(stats));
    assert(stats.stats_version == SDRN_ASYNC_STATS_VERSION);
    assert(stats.running == 1u);
    assert(stats.transfer_callbacks > 0u);
    assert(stats.transfers_completed > 0u);
    assert(stats.transfer_bytes > 0u);
    assert(stats.buffers_enqueued > 0u);
    assert(stats.consumer_reads > 0u);
    assert(stats.buffers_dropped_oldest > 0u);
    assert(stats.buffers_dropped_newest == 0u);
    assert(stats.queue_high_water == cfg.queue_depth);

    /* DROP_OLDEST advances past unread producer buffers */
    size_t after_drop_got = 0u;
    uint64_t after_drop_sequence = 0u;
    assert(sdrn_async_read(
        dev, buf, sizeof(buf), &after_drop_got, &after_drop_sequence, 1000u
    ) == SDRN_OK);
    assert(after_drop_got > 0u);
    assert(after_drop_sequence > sequence);

    assert(sdrn_async_stop(dev) == SDRN_OK);
    assert(sdrn_async_is_running(dev) == 0);
    assert(sdrn_async_get_stats(dev, &stats) == SDRN_OK);
    assert(stats.running == 0u);
    assert(stats.buffers_discarded_on_stop <= cfg.queue_depth);
    assert(sdrn_async_reset_stats(dev) == SDRN_OK);
    assert(sdrn_async_get_stats(dev, &stats) == SDRN_OK);
    assert(stats.transfer_callbacks == 0u);

    assert(sdrn_stop(dev) == SDRN_OK);
    sdrn_close(dev);
}

static void test_dvbt_async_partial_reads(void) {
    sdrn_device *dev = open_mock();
    sdrn_dvbt_tune_info tune;
    assert(sdrn_dvbt_start(dev, 557500000ull, 7000000u, &tune) == SDRN_OK);
    uint8_t stage = 0u;
    assert(sdrn_dvbt_wait_lock(dev, 100u, &stage) == SDRN_OK);
    assert(stage == 11u);

    sdrn_async_config cfg;
    sdrn_async_config_init(&cfg);
    cfg.transfer_size_bytes = 4096u;
    cfg.transfer_count = 3u;
    cfg.queue_depth = 3u;
    cfg.overflow_policy = SDRN_ASYNC_DROP_NEWEST;
    assert(sdrn_async_start(dev, &cfg) == SDRN_OK);

    unsigned char part1[188u * 4u];
    unsigned char part2[188u * 4u];
    size_t got1 = 0u, got2 = 0u;
    uint64_t seq1 = UINT64_MAX, seq2 = UINT64_MAX;
    assert(sdrn_async_read(dev, part1, sizeof(part1), &got1, &seq1, 1000u) == SDRN_OK);
    assert(got1 == sizeof(part1));
    assert(part1[0] == 0x47u);
    assert(sdrn_async_read(dev, part2, sizeof(part2), &got2, &seq2, 1000u) == SDRN_OK);
    assert(got2 == sizeof(part2));
    assert(seq2 == seq1); /* same producer buffer consumed in pieces */
    assert(part2[0] == 0x47u);

    sdrn_async_stats stats;
    wait_for_overflow(dev, SDRN_ASYNC_DROP_NEWEST, 2000u, &stats);
    assert(stats.buffers_dropped_newest > 0u);
    assert(stats.buffers_dropped_oldest == 0u);

    /* dropped newest buffers still consume producer sequence ids */
    /* consumer sees gap after retained queue drains */
    uint64_t gap_before = 0u;
    uint64_t gap_after = 0u;
    assert(read_until_sequence_gap(dev, seq2, &gap_before, &gap_after));
    assert(gap_after > gap_before + 1u);

    assert(sdrn_async_stop(dev) == SDRN_OK);
    assert(sdrn_stop(dev) == SDRN_OK);
    sdrn_close(dev);
}

static void test_config_validation(void) {
    sdrn_device *dev = open_mock();
    sdrn_iq_tune_info iq;
    assert(sdrn_iq_start(dev, 100700000ull, 2048000u, 0u, &iq) == SDRN_OK);

    sdrn_async_config cfg;
    sdrn_async_config_init(&cfg);
    cfg.transfer_size_bytes = 1024u;
    assert(sdrn_async_start(dev, &cfg) == SDRN_ERR_INVALID_ARGUMENT);

    sdrn_async_config_init(&cfg);
    cfg.overflow_policy = 99;
    assert(sdrn_async_start(dev, &cfg) == SDRN_ERR_INVALID_ARGUMENT);

    /* sdrn_stop owns async cancellation before hardware restore */
    sdrn_async_config_init(&cfg);
    cfg.transfer_size_bytes = 4096u;
    cfg.transfer_count = 2u;
    cfg.queue_depth = 2u;
    assert(sdrn_async_start(dev, &cfg) == SDRN_OK);
    assert(sdrn_async_start(dev, &cfg) == SDRN_ERR_STATE);
    assert(sdrn_async_reset_stats(dev) == SDRN_ERR_STATE);
    assert(sdrn_stop(dev) == SDRN_OK);
    assert(sdrn_async_is_running(dev) == 0);
    assert(sdrn_get_state(dev) == SDRN_STATE_IDLE);
    sdrn_async_stats stats;
    assert(sdrn_async_get_stats(dev, &stats) == SDRN_OK);
    assert(stats.running == 0u);

    sdrn_close(dev);
}

int main(void) {
    test_iq_async();
    test_dvbt_async_partial_reads();
    test_config_validation();
    puts("async: pass");
    return 0;
}
