/*
 * SDRNative
 * Core runtime tests
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

static void test_log(void *user, sdrn_log_level level, const char *message) {
    (void)user;
    if (level <= SDRN_LOG_DEBUG) fprintf(stderr, "[mock] %s\n", message);
}

static void check_device_profile(sdrn_device *dev) {
    sdrn_device_info info;
    assert(sdrn_get_device_info(dev, &info) == SDRN_OK);
    assert(info.vid == 0x0BDA);
    assert(info.pid == 0x2838);

    sdrn_capabilities caps;
    assert(sdrn_get_capabilities(dev, &caps) == SDRN_OK);
    assert(sdrn_get_state(dev) == SDRN_STATE_IDLE);
    assert(caps.flags & SDRN_CAP_DVBT);
    assert(caps.flags & SDRN_CAP_IQ);
    assert(caps.flags & SDRN_CAP_R820T_FAMILY);
    assert(caps.dvbt_bandwidth_mask & (1u << 8));

    sdrn_device_profile profile;
    assert(sdrn_get_device_profile(dev, &profile) == SDRN_OK);
    assert(profile.profile_version == SDRN_DEVICE_PROFILE_VERSION);
    assert((profile.feature_flags & SDRN_CAP_DVBT) != 0u);
    assert((profile.feature_flags & SDRN_CAP_IQ) != 0u);
    assert((profile.feature_flags & SDRN_CAP_R820T_FAMILY) == 0u);
    assert(profile.tuner_family == SDRN_TUNER_R820T_FAMILY);
    assert(profile.demod_family == SDRN_DEMOD_RTL2832);
    assert(strcmp(sdrn_tuner_family_string((sdrn_tuner_family)profile.tuner_family), "r820t-family") == 0);
    assert(strcmp(sdrn_demod_family_string((sdrn_demod_family)profile.demod_family), "rtl2832") == 0);
    assert(profile.rf_range_count == 1u);
    assert(profile.rf_ranges[0].min_hz == 24000000ull);
    assert(profile.rf_ranges[0].max_hz == 1766000000ull);
    assert(profile.iq_sample_rate_range_count == 2u);
    assert(profile.iq_sample_rate_ranges[0].min_hz == 225001u);
    assert(profile.iq_sample_rate_ranges[0].max_hz == 300000u);
    assert(profile.iq_sample_rate_ranges[1].min_hz == 900001u);
    assert(profile.iq_sample_rate_ranges[1].max_hz == 3200000u);
    assert(profile.iq_bandwidth_range_count == 1u);
    assert(profile.iq_bandwidth_ranges[0].min_hz == 225000u);
    assert(profile.iq_bandwidth_ranges[0].max_hz == 8000000u);
    assert(profile.dvbt_bandwidth_count == 3u);
    assert(profile.dvbt_bandwidth_hz[0] == 6000000u);
    assert(profile.dvbt_bandwidth_hz[1] == 7000000u);
    assert(profile.dvbt_bandwidth_hz[2] == 8000000u);
}

static void check_usb_accounting(sdrn_device *dev) {
    sdrn_usb_stats usb_stats;
    assert(sdrn_get_usb_stats(dev, &usb_stats) == SDRN_OK);
    assert(usb_stats.control_transfers > 0u);
    assert(usb_stats.bulk_reads > 0u);
    assert(usb_stats.bulk_bytes > 0u);
    assert(usb_stats.bulk_timeouts >= 1u);
    sdrn_usb_recovery_stats recovery_stats;
    memset(&recovery_stats, 0, sizeof(recovery_stats));
    assert(sdrn_get_usb_recovery_stats(dev, &recovery_stats) == SDRN_OK);
    assert(recovery_stats.struct_size == sizeof(recovery_stats));
    assert(recovery_stats.stats_version == SDRN_USB_RECOVERY_STATS_VERSION);
    assert(usb_stats.control_pipe_events == 0u);
    assert(usb_stats.session_recovery_attempts == 0u);
    assert(recovery_stats.control_pipe_events == 0u);
    assert(recovery_stats.session_reopen_attempts == 0u);
    assert(recovery_stats.transaction_retry_attempts == 0u);
    assert(sdrn_reset_usb_stats(dev) == SDRN_OK);
    assert(sdrn_get_usb_stats(dev, &usb_stats) == SDRN_OK);
    assert(usb_stats.control_transfers == 0u);
    assert(usb_stats.bulk_reads == 0u);
    memset(&recovery_stats, 0xA5, sizeof(recovery_stats));
    assert(sdrn_get_usb_recovery_stats(dev, &recovery_stats) == SDRN_OK);
    assert(recovery_stats.struct_size == sizeof(recovery_stats));
    assert(recovery_stats.stats_version == SDRN_USB_RECOVERY_STATS_VERSION);
    assert(recovery_stats.control_pipe_events == 0u);
    assert(recovery_stats.session_reopen_attempts == 0u);
    assert(recovery_stats.session_reopen_successes == 0u);
    assert(recovery_stats.session_reopen_failures == 0u);
    assert(recovery_stats.transaction_retry_attempts == 0u);
    assert(recovery_stats.transaction_retry_successes == 0u);
    assert(recovery_stats.transaction_retry_failures == 0u);
}

int main(void) {
    const sdrn_usb_ops *ops = NULL;
    void *ctx = NULL;
    assert(sdrn_test_mock_backend_create(&ops, &ctx) == SDRN_OK);

    sdrn_device *dev = NULL;
    assert(sdrn_device_create_with_backend(ops, ctx, 0, &dev) == SDRN_OK);
    assert(dev != NULL);
    assert(sdrn_last_global_error() != NULL);
    assert(strcmp(sdrn_error_domain_string(SDRN_ERROR_DOMAIN_USB), "usb") == 0);
    sdrn_error_info global_error_info;
    assert(sdrn_get_last_global_error_info(&global_error_info) == SDRN_OK);
    assert(global_error_info.status == SDRN_OK);
    assert(global_error_info.domain == SDRN_ERROR_DOMAIN_NONE);
    sdrn_usb_ops incomplete_ops = *ops;
    incomplete_ops.control_transfer = NULL;
    sdrn_device *invalid_dev = NULL;
    assert(sdrn_device_create_with_backend(&incomplete_ops, ctx, 0, &invalid_dev) == SDRN_ERR_INVALID_ARGUMENT);
    assert(invalid_dev == NULL);
    sdrn_set_log_callback(dev, test_log, NULL, SDRN_LOG_DEBUG);

    /* hardware delays are minimum duration contracts */
    uint64_t sleep_probe_started = sdrn_now_ms();
    sdrn_sleep_ms(5u);
    uint64_t sleep_probe_elapsed = sdrn_now_ms() - sleep_probe_started;
    assert(sleep_probe_elapsed >= 5u);

    check_device_profile(dev);

    /* iq mode must restore every demod register it changes */
    const uint8_t iq_seed_p1_01 = 0x14u;
    const uint8_t iq_seed_p1_04 = 0xA5u;
    const uint8_t iq_seed_p1_11 = 0x5Au;
    const uint8_t iq_seed_p1_1c = 0x77u;
    const uint8_t iq_seed_p1_9f = 0x66u;
    const uint8_t iq_seed_p1_b1 = 0xC3u;
    const uint8_t iq_seed_p0_19 = 0xE1u;
    assert(sdrn_demod_write(dev, 1u, 0x01u, &iq_seed_p1_01, 1u) == SDRN_OK);
    assert(sdrn_demod_write(dev, 1u, 0x04u, &iq_seed_p1_04, 1u) == SDRN_OK);
    assert(sdrn_demod_write(dev, 1u, 0x11u, &iq_seed_p1_11, 1u) == SDRN_OK);
    assert(sdrn_demod_write(dev, 1u, 0x1Cu, &iq_seed_p1_1c, 1u) == SDRN_OK);
    assert(sdrn_demod_write(dev, 1u, 0x9Fu, &iq_seed_p1_9f, 1u) == SDRN_OK);
    assert(sdrn_demod_write(dev, 1u, 0xB1u, &iq_seed_p1_b1, 1u) == SDRN_OK);
    assert(sdrn_demod_write(dev, 0u, 0x19u, &iq_seed_p0_19, 1u) == SDRN_OK);

    /* iq mode stays on SDRNative device handle */
    sdrn_iq_tune_info iq;
    assert(sdrn_iq_start(dev, 100700000ull, 2400000u, 0u, &iq) == SDRN_OK);
    assert(sdrn_get_mode(dev) == SDRN_MODE_IQ);
    assert(sdrn_get_state(dev) == SDRN_STATE_IQ);
    assert(iq.rf_hz == 100700000ull);
    assert(iq.requested_sample_rate_hz == 2400000u);
    assert(iq.actual_sample_rate_hz > 2390000u && iq.actual_sample_rate_hz < 2410000u);
    assert(iq.bandwidth_hz == 2400000u);
    assert(iq.tuner_pll_locked == 1u);
    assert(sdrn_test_mock_compact_i2c_write_count(ctx) > 0u);
    assert(sdrn_test_mock_direct_i2c_write_count(ctx) == 0u);

    sdrn_stream_info si;
    assert(sdrn_get_stream_info(dev, &si) == SDRN_OK);
    assert(si.format == SDRN_STREAM_U8_IQ_INTERLEAVED);
    assert(si.sample_rate_hz == iq.actual_sample_rate_hz);
    assert(si.bytes_per_complex_sample == 2u);

    uint8_t iqbuf[4096];
    size_t iqgot = 0;
    assert(sdrn_stream_read(dev, iqbuf, sizeof(iqbuf), &iqgot, 1000) == SDRN_OK);
    assert(iqgot == sizeof(iqbuf));
    assert(iqbuf[0] != iqbuf[1]);

    assert(sdrn_iq_tune(dev, 101100000ull, &iq) == SDRN_OK);
    assert(iq.rf_hz == 101100000ull);

    uint32_t actual_rate = 0;
    assert(sdrn_iq_set_sample_rate(dev, 2048000u, &actual_rate) == SDRN_OK);
    assert(actual_rate > 2040000u && actual_rate < 2055000u);
    /* default iq bandwidth follows sample rate */
    assert(sdrn_get_stream_info(dev, &si) == SDRN_OK);
    assert(si.sample_rate_hz == actual_rate);
    assert(sdrn_iq_set_bandwidth(dev, 1200000u, &iq) == SDRN_OK);
    assert(iq.bandwidth_hz == 1200000u);
    assert(iq.rf_hz == 101100000ull);

    assert(sdrn_stop(dev) == SDRN_OK);
    assert(sdrn_get_mode(dev) == SDRN_MODE_NONE);
    assert(sdrn_get_state(dev) == SDRN_STATE_IDLE);

    uint8_t iq_restored = 0u;
    assert(sdrn_demod_read(dev, 1u, 0x01u, &iq_restored, 1u) == SDRN_OK); assert(iq_restored == iq_seed_p1_01);
    assert(sdrn_demod_read(dev, 1u, 0x04u, &iq_restored, 1u) == SDRN_OK); assert(iq_restored == iq_seed_p1_04);
    assert(sdrn_demod_read(dev, 1u, 0x11u, &iq_restored, 1u) == SDRN_OK); assert(iq_restored == iq_seed_p1_11);
    assert(sdrn_demod_read(dev, 1u, 0x1Cu, &iq_restored, 1u) == SDRN_OK); assert(iq_restored == iq_seed_p1_1c);
    assert(sdrn_demod_read(dev, 1u, 0x9Fu, &iq_restored, 1u) == SDRN_OK); assert(iq_restored == iq_seed_p1_9f);
    assert(sdrn_demod_read(dev, 1u, 0xB1u, &iq_restored, 1u) == SDRN_OK); assert(iq_restored == iq_seed_p1_b1);
    assert(sdrn_demod_read(dev, 0u, 0x19u, &iq_restored, 1u) == SDRN_OK); assert(iq_restored == iq_seed_p0_19);

    /* raw access works while device is idle */
    uint8_t sysctl = 0;
    assert(sdrn_reg_read(dev, SDRN_USB_SYSCTL_0, &sysctl, 1) == SDRN_OK);
    assert(sysctl == 0x01);

    /* raw vendor control reaches RTL28xx map */
    uint8_t sysctl_raw = 0;
    assert(sdrn_control_read(dev, SDRN_USB_SYSCTL_0, SDRN_CMD_USB_RD, &sysctl_raw, 1) == SDRN_OK);
    assert(sysctl_raw == 0x01);

    assert(sdrn_i2c_gate_open(dev) == SDRN_OK);
    const uint8_t direct_write[] = {0x20u, 0x5Au};
    assert(sdrn_i2c_write_direct(dev, SDRN_R820T_ADDR, direct_write, sizeof(direct_write)) == SDRN_OK);
    const uint8_t direct_pointer = 0x20u;
    assert(sdrn_i2c_write_direct(dev, SDRN_R820T_ADDR, &direct_pointer, 1u) == SDRN_OK);
    uint8_t direct_read = 0u;
    assert(sdrn_i2c_read_direct(dev, SDRN_R820T_ADDR, &direct_read, 1u) == SDRN_OK);
    assert(direct_read == 0x5Au);
    assert(sdrn_i2c_gate_close(dev) == SDRN_OK);

    sdrn_dvbt_tune_info tune;
    /* dvb tuning follows R820T RF range without broadcast plan limits */
    assert(sdrn_dvbt_start(dev, 23999999ull, 7000000u, &tune) == SDRN_ERR_INVALID_ARGUMENT);
    sdrn_error_info error_info;
    assert(sdrn_get_last_error_info(dev, &error_info) == SDRN_OK);
    assert(error_info.status == SDRN_ERR_INVALID_ARGUMENT);
    assert(error_info.domain == SDRN_ERROR_DOMAIN_ARGUMENT);
    assert(error_info.native_code == 0);
    assert(strstr(error_info.message, "frequency") != NULL);
    assert(strstr(sdrn_last_error(dev), "frequency") != NULL);
    assert(sdrn_get_mode(dev) == SDRN_MODE_NONE);
    assert(sdrn_dvbt_start(dev, 1766000001ull, 7000000u, &tune) == SDRN_ERR_INVALID_ARGUMENT);
    assert(sdrn_get_mode(dev) == SDRN_MODE_NONE);
    /* 100 mhz stays valid at low level even outside normal broadcast plans */
    /* fresh dvb start writes 0x0C and 0x13 once in init and once in TV config */
    unsigned pre_0c = sdrn_test_mock_tuner_reg_write_count(ctx, 0x0Cu);
    unsigned pre_13 = sdrn_test_mock_tuner_reg_write_count(ctx, 0x13u);
    unsigned pre_gate_open = sdrn_test_mock_tuner_gate_open_count(ctx);
    unsigned pre_gate_close = sdrn_test_mock_tuner_gate_close_count(ctx);
    assert(sdrn_dvbt_start(dev, 100000000ull, 7000000u, &tune) == SDRN_OK);
    assert(sdrn_test_mock_tuner_reg_write_count(ctx, 0x0Cu) - pre_0c == 2u);
    assert(sdrn_test_mock_tuner_reg_write_count(ctx, 0x13u) - pre_13 == 2u);
    /* tuner gate stays closed during 250 ms LNA settle */
    assert(sdrn_test_mock_tuner_gate_open_count(ctx) - pre_gate_open >= 2u);
    assert(sdrn_test_mock_tuner_gate_close_count(ctx) - pre_gate_close >= 2u);
    assert(tune.rf_hz == 100000000ull);
    assert(sdrn_test_mock_compact_i2c_write_count(ctx) > 0u);
    assert(sdrn_stop(dev) == SDRN_OK);
    uint8_t dvbt_stopped_reg01 = 0u;
    assert(sdrn_demod_read(dev, 1u, 0x01u, &dvbt_stopped_reg01, 1u) == SDRN_OK);
    assert((dvbt_stopped_reg01 & 0x04u) != 0u);
    assert(sdrn_dvbt_start(dev, 557500000ull, 7000000u, &tune) == SDRN_OK);
    assert(sdrn_get_mode(dev) == SDRN_MODE_DVBT);
    assert(sdrn_get_state(dev) == SDRN_STATE_DVBT);
    assert(tune.rf_hz == 557500000ull);
    assert(tune.bandwidth_hz == 7000000u);
    assert(tune.if_hz == 4570000u);
    assert(tune.lo_hz == 562070000ull);
    assert(tune.tuner_pll_locked == 1);
    assert(tune.filter_cal_code == 7);

    assert(sdrn_iq_start(dev, 100700000ull, 2400000u, 0u, &iq) == SDRN_ERR_STATE);
    assert(sdrn_get_last_error_info(dev, &error_info) == SDRN_OK);
    assert(error_info.status == SDRN_ERR_STATE);
    assert(error_info.domain == SDRN_ERROR_DOMAIN_STATE);
    assert(strstr(error_info.message, "already in use") != NULL);

    uint8_t stage = 0;
    assert(sdrn_dvbt_wait_lock(dev, 100, &stage) == SDRN_OK);
    assert(stage == 11);

    sdrn_signal_quality q;
    assert(sdrn_dvbt_get_signal_quality(dev, &q) == SDRN_OK);
    assert(q.fsm_stage == 11);
    assert(q.snr_valid == 1);
    assert(q.ber == 3);
    assert(q.ber_valid == 1);

    uint16_t pids[] = {0x0000, 0x0066, 0x0067, 0x0068, 0x0400};
    assert(sdrn_dvbt_set_pid_filter(dev, pids, 5) == SDRN_OK);

    uint8_t buffer[188 * 8];
    size_t got = 0;
    assert(sdrn_stream_read(dev, buffer, sizeof(buffer), &got, 1000) == SDRN_OK);
    assert(got == sizeof(buffer));
    for (size_t i = 0; i < got; i += 188) assert(buffer[i] == 0x47);

    /* timeout can still carry useful partial bytes */
    got = 0;
    assert(sdrn_stream_read(dev, buffer, sizeof(buffer), &got, 123) == SDRN_OK);
    assert(got == 188);
    assert(buffer[0] == 0x47);

    /* retune keeps native mode active */
    assert(sdrn_dvbt_tune(dev, 650000000ull, 8000000u, &tune) == SDRN_OK);
    assert(tune.bandwidth_hz == 8000000u);
    assert(tune.if_hz == 4570000u);

    /* 6 mhz uses 3.570 mhz IF */
    assert(sdrn_dvbt_tune(dev, 177500000ull, 6000000u, &tune) == SDRN_OK);
    assert(tune.bandwidth_hz == 6000000u);
    assert(tune.if_hz == 3570000u);

    /* invalid dvb bandwidth must not tear mode down */
    assert(sdrn_dvbt_tune(dev, 177500000ull, 5000000u, &tune) == SDRN_ERR_INVALID_ARGUMENT);
    assert(sdrn_get_mode(dev) == SDRN_MODE_DVBT);

    assert(sdrn_stop(dev) == SDRN_OK);
    assert(sdrn_get_mode(dev) == SDRN_MODE_NONE);

    /* same handle must survive dvb to iq to dvb */
    /* second dvb start still reaches lock and transport */
    assert(sdrn_iq_start(dev, 100700000ull, 2400000u, 0u, &iq) == SDRN_OK);
    iqgot = 0u;
    assert(sdrn_stream_read(dev, iqbuf, sizeof(iqbuf), &iqgot, 1000u) == SDRN_OK);
    assert(iqgot == sizeof(iqbuf));
    assert(sdrn_stop(dev) == SDRN_OK);

    assert(sdrn_dvbt_start(dev, 557500000ull, 7000000u, &tune) == SDRN_OK);
    stage = 0u;
    assert(sdrn_dvbt_wait_lock(dev, 100u, &stage) == SDRN_OK);
    assert(stage == 11u);
    got = 0u;
    assert(sdrn_stream_read(dev, buffer, sizeof(buffer), &got, 1000u) == SDRN_OK);
    assert(got == sizeof(buffer));
    for (size_t i = 0; i < got; i += 188u) assert(buffer[i] == 0x47u);
    assert(sdrn_stop(dev) == SDRN_OK);

    /* restore pre mode SYS state */
    sysctl = 0;
    assert(sdrn_reg_read(dev, SDRN_USB_SYSCTL_0, &sysctl, 1) == SDRN_OK);
    assert(sysctl == 0x01);
    assert(sdrn_get_state(dev) == SDRN_STATE_IDLE);

    check_usb_accounting(dev);

    sdrn_close(dev);
    puts("device: pass");
    return 0;
}