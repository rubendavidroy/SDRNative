/*
 * SDRNative
 * Internal interfaces
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef SDRNATIVE_INTERNAL_H
#define SDRNATIVE_INTERNAL_H

#include "sdrnative.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDRN_REALTEK_VID 0x0BDAu
#define SDRN_RTL2832_PID_1 0x2832u
#define SDRN_RTL2832_PID_2 0x2838u
#define SDRN_DEFAULT_INTERFACE 0
#define SDRN_CTRL_TIMEOUT_MS 1000u
#define SDRN_TS_ENDPOINT 0x81u

#define SDRN_CMD_DEMOD_RD 0x0000u
#define SDRN_CMD_DEMOD_WR 0x0010u
#define SDRN_CMD_USB_RD   0x0100u
#define SDRN_CMD_USB_WR   0x0110u
#define SDRN_CMD_SYS_RD   0x0200u
#define SDRN_CMD_SYS_WR   0x0210u
#define SDRN_CMD_IR_RD    0x0201u
#define SDRN_CMD_IR_WR    0x0211u
#define SDRN_CMD_I2C_RD   0x0300u
#define SDRN_CMD_I2C_WR   0x0310u
#define SDRN_CMD_I2C_DA_RD 0x0600u
#define SDRN_CMD_I2C_DA_WR 0x0610u

#define SDRN_USB_SYSCTL_0 0x2000u
#define SDRN_USB_EPA_CTL 0x2148u
#define SDRN_USB_EPA_MAXPKT 0x2158u
#define SDRN_USB_EPA_FIFO_CFG 0x2160u

#define SDRN_SYS_DEMOD_CTL 0x3000u
#define SDRN_SYS_GPIO_OUT_VAL 0x3001u
#define SDRN_SYS_GPIO_IN_VAL 0x3002u
#define SDRN_SYS_GPIO_OUT_EN 0x3003u
#define SDRN_SYS_GPIO_DIR 0x3004u
#define SDRN_SYS_DEMOD_CTL1 0x300Bu

#define SDRN_I2C_GATE_PAGE 1u
#define SDRN_I2C_GATE_REG 0x01u
#define SDRN_I2C_GATE_OPEN 0x18u
#define SDRN_I2C_GATE_CLOSE 0x10u

#define SDRN_R820T_ADDR 0x1Au
#define SDRN_R820T_ID 0x69u
#define SDRN_XTAL_HZ 28800000u

/* bmRequestType values for vendor device transfers */
#define SDRN_USB_REQ_IN  0xC0u
#define SDRN_USB_REQ_OUT 0x40u

struct sdrn_usb_ops;

typedef struct sdrn_power_snapshot {
    uint8_t gpio_out;
    uint8_t gpio_dir;
    uint8_t gpio_out_en;
    uint8_t demod_ctl1;
    uint8_t demod_ctl;
    uint8_t epa_ctl[2];
    bool valid;
} sdrn_power_snapshot;

typedef struct sdrn_ts_snapshot {
    uint8_t sysctl0;
    uint8_t maxpkt[4];
    uint8_t fifo_cfg[4];
    bool valid;
} sdrn_ts_snapshot;

typedef struct sdrn_iq_demod_snapshot {
    uint8_t page0_reg06;
    uint8_t page0_reg0d;
    uint8_t page0_reg19;
    uint8_t page0_reg61;
    uint8_t page1_reg01;
    uint8_t page1_reg04;
    uint8_t page1_reg11;
    uint8_t page1_15_1b[0x1B - 0x15 + 1];
    uint8_t page1_1c_2b[0x2B - 0x1C + 1];
    uint8_t page1_3e_3f[2];
    uint8_t page1_93_94[2];
    uint8_t page1_9f_a2[0xA2 - 0x9F + 1];
    uint8_t page1_b1;
    bool valid;
} sdrn_iq_demod_snapshot;

typedef struct sdrn_r820t_state {
    uint8_t regs[0x1F - 0x05 + 1];
    bool initialized;
    bool imr_done;
    uint32_t if_hz;
    uint32_t bandwidth_hz;
    uint8_t filter_cal_code;
    bool has_lock;
} sdrn_r820t_state;

typedef struct sdrn_thread sdrn_thread;
typedef struct sdrn_mutex sdrn_mutex;
typedef struct sdrn_cond sdrn_cond;
typedef int (*sdrn_thread_fn)(void *user);

typedef void (*sdrn_usb_async_completion_fn)(
    void *user,
    sdrn_status status,
    int32_t native_code,
    const uint8_t *data,
    size_t length,
    const char *error
);

typedef struct sdrn_async_state sdrn_async_state;

typedef struct sdrn_usb_ops {
    void (*destroy)(void *ctx);
    sdrn_status (*list)(
        void *ctx,
        sdrn_device_info *infos,
        size_t capacity,
        size_t *count,
        char *error,
        size_t error_cap
    );
    sdrn_status (*open)(
        void *ctx,
        size_t index,
        void **out_handle,
        sdrn_device_info *out_info,
        char *error,
        size_t error_cap
    );
    void (*close)(void *ctx, void *handle);
    sdrn_status (*claim_interface)(
        void *ctx, void *handle, int interface_number,
        char *error, size_t error_cap
    );
    void (*release_interface)(void *ctx, void *handle, int interface_number);
    sdrn_status (*control_transfer)(
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
    );
    sdrn_status (*bulk_read)(
        void *ctx,
        void *handle,
        uint8_t endpoint,
        uint8_t *data,
        int length,
        uint32_t timeout_ms,
        int *transferred,
        char *error,
        size_t error_cap
    );
    sdrn_status (*clear_halt)(
        void *ctx,
        void *handle,
        uint8_t endpoint,
        char *error,
        size_t error_cap
    );
    /* recover sticky host control session without resetting usb device */
    /* implementation may replace handle while preserving physical state */
    sdrn_status (*recover_session)(
        void *ctx,
        void **handle,
        int interface_number,
        const sdrn_device_info *expected_info,
        char *error,
        size_t error_cap
    );
    sdrn_status (*endpoint_info)(
        void *ctx,
        void *handle,
        int interface_number,
        uint8_t endpoint,
        uint16_t *max_packet_size,
        bool *is_bulk,
        char *error,
        size_t error_cap
    );
    sdrn_status (*async_bulk_start)(
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
    );
    /* stop backend transfer engine synchronously */
    /* no completion callback may run after return */
    sdrn_status (*async_bulk_stop)(
        void *ctx,
        void *async_handle,
        char *error,
        size_t error_cap
    );
    /* backend native error from most recent operation */
    /* libusb uses negative LIBUSB_ERROR values */
    int32_t (*native_error_code)(void *ctx);
} sdrn_usb_ops;

struct sdrn_device {
    const sdrn_usb_ops *usb;
    void *usb_ctx;
    void *usb_handle;
    bool interface_claimed;
    bool device_gone;
    sdrn_device_info info;

    sdrn_mode mode;
    bool i2c_gate_open;
    bool frontend_initialized;
    bool tuner_initialized;
    bool ts_configured;
    bool power_configured;
    /* set after endpoint 0x81 returns stream data */
    /* later retune uses clean native mode restart */
    bool stream_consumed;
    /* set once endpoint 0x81 has any armed host bulk transfer */
    /* zero completed bytes still require stream to control boundary */
    bool stream_transport_armed;
    bool iq_configured;
    bool dvbt_post_lock_flush_done;

    sdrn_power_snapshot power_snapshot;
    sdrn_ts_snapshot ts_snapshot;
    sdrn_iq_demod_snapshot iq_demod_snapshot;
    sdrn_r820t_state tuner;
    sdrn_dvbt_tune_info tune;
    sdrn_iq_tune_info iq_tune;
    sdrn_usb_stats usb_stats;
    sdrn_usb_recovery_stats usb_recovery_stats;
    /* protects async_state lifetime across reader and stop */
    /* other same handle operations remain caller synchronized */
    sdrn_mutex *async_lifecycle_mutex;
    sdrn_async_state *async_state;
    sdrn_async_stats last_async_stats;
    bool last_async_stats_valid;

    sdrn_log_fn log_fn;
    void *log_user;
    sdrn_log_level log_minimum;
    char last_error[512];
    sdrn_error_info last_error_info;
};

/* usb backend */
sdrn_status sdrn_libusb_backend_create(
    const sdrn_usb_ops **out_ops,
    void **out_ctx,
    char *error,
    size_t error_cap
);

/* core helpers */
void sdrn_clear_error(sdrn_device *dev);
void sdrn_clear_global_error(void);
void sdrn_set_errorf(sdrn_device *dev, const char *fmt, ...);
void sdrn_set_error_infof(
    sdrn_device *dev, sdrn_status status, sdrn_error_domain domain,
    int32_t native_code, uint32_t flags, const char *fmt, ...
);
void sdrn_set_global_errorf(const char *fmt, ...);
void sdrn_set_global_error_infof(
    sdrn_status status, sdrn_error_domain domain, int32_t native_code,
    uint32_t flags, const char *fmt, ...
);
void sdrn_logf(sdrn_device *dev, sdrn_log_level level, const char *fmt, ...);
void sdrn_sleep_ms(uint32_t milliseconds);
uint64_t sdrn_now_ms(void);

sdrn_status sdrn_thread_create(sdrn_thread **out_thread, sdrn_thread_fn fn, void *user);
void sdrn_thread_join_destroy(sdrn_thread *thread);
sdrn_status sdrn_mutex_create(sdrn_mutex **out_mutex);
void sdrn_mutex_destroy(sdrn_mutex *mutex);
void sdrn_mutex_lock(sdrn_mutex *mutex);
void sdrn_mutex_unlock(sdrn_mutex *mutex);
sdrn_status sdrn_cond_create(sdrn_cond **out_cond);
void sdrn_cond_destroy(sdrn_cond *cond);
void sdrn_cond_signal(sdrn_cond *cond);
void sdrn_cond_broadcast(sdrn_cond *cond);
/* 0 signaled 1 timeout minus 1 error UINT32_MAX waits forever */
int sdrn_cond_wait_ms(sdrn_cond *cond, sdrn_mutex *mutex, uint32_t timeout_ms);

bool sdrn_async_stream_active(const sdrn_device *dev);
bool sdrn_async_terminal_snapshot(
    const sdrn_device *dev,
    sdrn_status *out_status,
    int32_t *out_native_code,
    char *out_error,
    size_t error_cap
);
sdrn_status sdrn_async_stop_internal(sdrn_device *dev, bool preserve_error);
#if defined(SDRNATIVE_TEST_HOOKS)
sdrn_status sdrn_async_test_set_next_sequence(sdrn_device *dev, uint64_t sequence);
uint32_t sdrn_async_test_active_readers(const sdrn_device *dev);
#endif


static inline int32_t sdrn_usb_native_error(const sdrn_device *dev) {
    if (!dev || !dev->usb || !dev->usb->native_error_code) return 0;
    return dev->usb->native_error_code(dev->usb_ctx);
}

/* raw transport */
sdrn_status sdrn_ctrl_read(
    sdrn_device *dev, uint16_t value, uint16_t index,
    uint8_t *data, size_t length
);
sdrn_status sdrn_ctrl_write(
    sdrn_device *dev, uint16_t value, uint16_t index,
    const uint8_t *data, size_t length
);
sdrn_status sdrn_reg_read_internal(
    sdrn_device *dev, uint16_t reg, uint8_t *data, size_t length
);
sdrn_status sdrn_reg_write_internal(
    sdrn_device *dev, uint16_t reg, const uint8_t *data, size_t length
);
sdrn_status sdrn_reg_write_mask_internal(
    sdrn_device *dev, uint16_t reg, uint8_t value, uint8_t mask,
    uint8_t *previous
);
sdrn_status sdrn_demod_read_internal(
    sdrn_device *dev, uint8_t page, uint8_t reg,
    uint8_t *data, size_t length
);
sdrn_status sdrn_demod_write_internal(
    sdrn_device *dev, uint8_t page, uint8_t reg,
    const uint8_t *data, size_t length
);
sdrn_status sdrn_i2c_read_reg_internal(
    sdrn_device *dev, uint8_t address, uint8_t reg,
    uint8_t *data, size_t length
);
sdrn_status sdrn_i2c_write_reg_internal(
    sdrn_device *dev, uint8_t address, uint8_t reg,
    const uint8_t *data, size_t length
);
sdrn_status sdrn_i2c_read_direct_internal(
    sdrn_device *dev, uint8_t address, uint8_t *data, size_t length
);
sdrn_status sdrn_i2c_write_direct_internal(
    sdrn_device *dev, uint8_t address, const uint8_t *data, size_t length
);
sdrn_status sdrn_i2c_gate_set(sdrn_device *dev, bool open);

sdrn_status sdrn_ts_snapshot_state(sdrn_device *dev, sdrn_ts_snapshot *snapshot);
sdrn_status sdrn_ts_configure(sdrn_device *dev);
sdrn_status sdrn_ts_restore(sdrn_device *dev);
sdrn_status sdrn_power_on_dvb(sdrn_device *dev);
sdrn_status sdrn_iq_usb_power_on(sdrn_device *dev);
sdrn_status sdrn_stream_reset_endpoint(sdrn_device *dev);
sdrn_status sdrn_stream_quiesce_endpoint(sdrn_device *dev);
sdrn_status sdrn_power_restore(sdrn_device *dev);

/* tuner */
void sdrn_r820t_reset_state(sdrn_device *dev);
sdrn_status sdrn_r820t_probe(sdrn_device *dev, uint8_t *out_id);
sdrn_status sdrn_r820t_initialize_fast(sdrn_device *dev);
sdrn_status sdrn_r820t_prepare_dvbt(sdrn_device *dev);
sdrn_status sdrn_r820t_tune_dvbt(
    sdrn_device *dev,
    uint64_t rf_hz,
    uint32_t bandwidth_hz,
    sdrn_dvbt_tune_info *out_tune
);
sdrn_status sdrn_r820t_tune_iq(
    sdrn_device *dev,
    uint64_t rf_hz,
    uint32_t bandwidth_hz,
    uint32_t *out_if_hz,
    uint64_t *out_lo_hz
);
sdrn_status sdrn_r820t_standby(sdrn_device *dev);

/* demod */
sdrn_status sdrn_rtl2832_attach(sdrn_device *dev);
sdrn_status sdrn_rtl2832_initialize_r820t(sdrn_device *dev);
sdrn_status sdrn_rtl2832_configure_bandwidth(
    sdrn_device *dev, uint32_t bandwidth_hz, uint32_t if_hz
);
sdrn_status sdrn_rtl2832_set_pid_filter(
    sdrn_device *dev, const uint16_t *pids, size_t count
);
sdrn_status sdrn_rtl2832_disable_pid_filter(sdrn_device *dev);
sdrn_status sdrn_rtl2832_get_fsm(sdrn_device *dev, uint8_t *out_stage);
sdrn_status sdrn_rtl2832_get_quality(
    sdrn_device *dev, sdrn_signal_quality *out_quality
);
sdrn_status sdrn_rtl2832_release(sdrn_device *dev);

/* raw iq mode */
sdrn_status sdrn_rtl2832_iq_snapshot_state(sdrn_device *dev);
sdrn_status sdrn_rtl2832_iq_restore_state(sdrn_device *dev);
sdrn_status sdrn_rtl2832_iq_initialize(sdrn_device *dev);
sdrn_status sdrn_rtl2832_iq_set_sample_rate(
    sdrn_device *dev, uint32_t requested_rate_hz, uint32_t *out_actual_rate_hz
);
sdrn_status sdrn_rtl2832_iq_set_if(sdrn_device *dev, uint32_t if_hz);

/* internal constructor for mock backend */
sdrn_status sdrn_device_create_with_backend(
    const sdrn_usb_ops *ops,
    void *ctx,
    size_t index,
    sdrn_device **out_device
);

#endif /* SDRNATIVE_INTERNAL_H */