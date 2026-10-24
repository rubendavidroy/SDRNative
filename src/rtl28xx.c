/*
 * SDRNative
 * RTL28xx USB and bridge control
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static sdrn_status checked_length(sdrn_device *dev, size_t length) {
    if (length == 0 || length > UINT16_MAX) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE, "invalid USB control transfer length %zu", length
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }
    return SDRN_OK;
}

static bool ctrl_recover_usb_session(sdrn_device *dev) {
#if defined(_WIN32) || defined(SDRNATIVE_TEST_SESSION_RECOVERY)
    if (!dev || !dev->usb || !dev->usb->recover_session) return false;

    char recovery_error[256] = {0};
    sdrn_status recovery = dev->usb->recover_session(
        dev->usb_ctx,
        &dev->usb_handle,
        SDRN_DEFAULT_INTERFACE,
        &dev->info,
        recovery_error,
        sizeof(recovery_error)
    );
    if (recovery != SDRN_ERR_UNSUPPORTED) {
        dev->usb_stats.control_pipe_events++;
        dev->usb_stats.session_recovery_attempts++;
        dev->usb_recovery_stats.control_pipe_events++;
        dev->usb_recovery_stats.session_reopen_attempts++;
        if (recovery == SDRN_OK) {
            dev->usb_stats.session_recovery_successes++;
            dev->usb_recovery_stats.session_reopen_successes++;
        } else {
            dev->usb_stats.session_recovery_failures++;
            dev->usb_recovery_stats.session_reopen_failures++;
        }
    }
    if (recovery == SDRN_OK) {
        sdrn_logf(
            dev, SDRN_LOG_INFO,
            "USB control session reopened after PIPE"
        );
        return true;
    }
    if (recovery != SDRN_ERR_UNSUPPORTED) {
        sdrn_logf(
            dev, SDRN_LOG_WARN,
            "USB control session reopen failed: %s",
            recovery_error[0] ? recovery_error : sdrn_status_string(recovery)
        );
    }
#else
    (void)dev;
#endif
    return false;
}

static sdrn_status ctrl_read_impl(
    sdrn_device *dev, uint16_t value, uint16_t index,
    uint8_t *data, size_t length, bool allow_session_recovery
) {
    if (!dev || !data) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st = checked_length(dev, length);
    if (st != SDRN_OK) return st;
    if (dev->device_gone) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_NOT_FOUND, SDRN_ERROR_DOMAIN_USB, 0,
            SDRN_ERROR_FLAG_DEVICE_GONE,
            "device disconnected; reopen the handle"
        );
        return SDRN_ERR_NOT_FOUND;
    }
    if (dev->async_state) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE,
            "stop async streaming before USB control access"
        );
        return SDRN_ERR_STATE;
    }

    int transferred = 0;
    char err[256] = {0};
    dev->usb_stats.control_transfers++;
    st = dev->usb->control_transfer(
        dev->usb_ctx, dev->usb_handle,
        SDRN_USB_REQ_IN, 0u, value, index,
        data, (uint16_t)length, SDRN_CTRL_TIMEOUT_MS,
        &transferred, err, sizeof(err)
    );
    if (st != SDRN_OK) dev->usb_stats.control_errors++;

    if (st != SDRN_OK && allow_session_recovery && ctrl_recover_usb_session(dev)) {
        sdrn_logf(
            dev, SDRN_LOG_DEBUG,
            "retrying USB control read after reopen "
            "(value=0x%04X index=0x%04X len=%zu)",
            value, index, length
        );
        transferred = 0;
        err[0] = '\0';
        dev->usb_recovery_stats.transaction_retry_attempts++;
        dev->usb_stats.control_transfers++;
        st = dev->usb->control_transfer(
            dev->usb_ctx, dev->usb_handle,
            SDRN_USB_REQ_IN, 0u, value, index,
            data, (uint16_t)length, SDRN_CTRL_TIMEOUT_MS,
            &transferred, err, sizeof(err)
        );
        if (st != SDRN_OK) {
            dev->usb_stats.control_errors++;
            dev->usb_recovery_stats.transaction_retry_failures++;
        } else if (transferred != (int)length) {
            dev->usb_recovery_stats.transaction_retry_failures++;
        } else {
            dev->usb_recovery_stats.transaction_retry_successes++;
        }
    }

    if (st != SDRN_OK) {
        uint32_t flags = SDRN_ERROR_FLAG_NONE;
        if (st == SDRN_ERR_NOT_FOUND) {
            dev->device_gone = true;
            flags |= SDRN_ERROR_FLAG_DEVICE_GONE;
        }
        sdrn_set_error_infof(
            dev, st, SDRN_ERROR_DOMAIN_USB, sdrn_usb_native_error(dev),
            flags,
            "USB control read failed (value=0x%04X index=0x%04X len=%zu): %s",
            value, index, length, err[0] ? err : sdrn_status_string(st)
        );
        return st;
    }
    if (transferred != (int)length) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_IO, SDRN_ERROR_DOMAIN_USB, 0, SDRN_ERROR_FLAG_NONE,
            "short USB control read (expected %zu, got %d)", length, transferred
        );
        return SDRN_ERR_IO;
    }
    sdrn_clear_error(dev);
    return SDRN_OK;
}

static sdrn_status ctrl_write_impl(
    sdrn_device *dev, uint16_t value, uint16_t index,
    const uint8_t *data, size_t length, bool allow_session_recovery
) {
    if (!dev || !data) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st = checked_length(dev, length);
    if (st != SDRN_OK) return st;
    if (dev->device_gone) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_NOT_FOUND, SDRN_ERROR_DOMAIN_USB, 0,
            SDRN_ERROR_FLAG_DEVICE_GONE,
            "device disconnected; reopen the handle"
        );
        return SDRN_ERR_NOT_FOUND;
    }
    if (dev->async_state) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE,
            "stop async streaming before USB control access"
        );
        return SDRN_ERR_STATE;
    }

    int transferred = 0;
    char err[256] = {0};
    dev->usb_stats.control_transfers++;
    st = dev->usb->control_transfer(
        dev->usb_ctx, dev->usb_handle,
        SDRN_USB_REQ_OUT, 0u, value, index,
        (uint8_t *)(uintptr_t)data, (uint16_t)length,
        SDRN_CTRL_TIMEOUT_MS,
        &transferred, err, sizeof(err)
    );
    if (st != SDRN_OK) dev->usb_stats.control_errors++;

    if (st != SDRN_OK && allow_session_recovery && ctrl_recover_usb_session(dev)) {
        sdrn_logf(
            dev, SDRN_LOG_DEBUG,
            "retrying USB control write after reopen "
            "(value=0x%04X index=0x%04X data0=0x%02X len=%zu)",
            value, index, (unsigned)data[0], length
        );
        transferred = 0;
        err[0] = '\0';
        dev->usb_recovery_stats.transaction_retry_attempts++;
        dev->usb_stats.control_transfers++;
        st = dev->usb->control_transfer(
            dev->usb_ctx, dev->usb_handle,
            SDRN_USB_REQ_OUT, 0u, value, index,
            (uint8_t *)(uintptr_t)data, (uint16_t)length,
            SDRN_CTRL_TIMEOUT_MS,
            &transferred, err, sizeof(err)
        );
        if (st != SDRN_OK) {
            dev->usb_stats.control_errors++;
            dev->usb_recovery_stats.transaction_retry_failures++;
        } else if (transferred != (int)length) {
            dev->usb_recovery_stats.transaction_retry_failures++;
        } else {
            dev->usb_recovery_stats.transaction_retry_successes++;
        }
    }

    if (st != SDRN_OK) {
        uint32_t flags = SDRN_ERROR_FLAG_NONE;
        if (st == SDRN_ERR_NOT_FOUND) {
            dev->device_gone = true;
            flags |= SDRN_ERROR_FLAG_DEVICE_GONE;
        }
        sdrn_set_error_infof(
            dev, st, SDRN_ERROR_DOMAIN_USB, sdrn_usb_native_error(dev),
            flags,
            "USB control write failed (value=0x%04X index=0x%04X len=%zu data0=0x%02X): %s",
            value, index, length, (unsigned)data[0],
            err[0] ? err : sdrn_status_string(st)
        );
        return st;
    }
    if (transferred != (int)length) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_IO, SDRN_ERROR_DOMAIN_USB, 0, SDRN_ERROR_FLAG_NONE,
            "short USB control write (expected %zu, got %d)", length, transferred
        );
        return SDRN_ERR_IO;
    }
    sdrn_clear_error(dev);
    return SDRN_OK;
}

sdrn_status sdrn_ctrl_read(
    sdrn_device *dev, uint16_t value, uint16_t index,
    uint8_t *data, size_t length
) {
    return ctrl_read_impl(dev, value, index, data, length, true);
}

sdrn_status sdrn_ctrl_write(
    sdrn_device *dev, uint16_t value, uint16_t index,
    const uint8_t *data, size_t length
) {
    return ctrl_write_impl(dev, value, index, data, length, true);
}

static uint16_t read_command_for_reg(uint16_t reg) {
    if (reg < 0x3000u) return SDRN_CMD_USB_RD;
    if (reg < 0x4000u) return SDRN_CMD_SYS_RD;
    return SDRN_CMD_IR_RD;
}

static uint16_t write_command_for_reg(uint16_t reg) {
    if (reg < 0x3000u) return SDRN_CMD_USB_WR;
    if (reg < 0x4000u) return SDRN_CMD_SYS_WR;
    return SDRN_CMD_IR_WR;
}

sdrn_status sdrn_reg_read_internal(
    sdrn_device *dev, uint16_t reg, uint8_t *data, size_t length
) {
    return sdrn_ctrl_read(dev, reg, read_command_for_reg(reg), data, length);
}

sdrn_status sdrn_reg_write_internal(
    sdrn_device *dev, uint16_t reg, const uint8_t *data, size_t length
) {
    return sdrn_ctrl_write(dev, reg, write_command_for_reg(reg), data, length);
}

sdrn_status sdrn_reg_write_mask_internal(
    sdrn_device *dev, uint16_t reg, uint8_t value, uint8_t mask,
    uint8_t *previous
) {
    uint8_t old = 0;
    sdrn_status st = sdrn_reg_read_internal(dev, reg, &old, 1);
    if (st != SDRN_OK) return st;
    if (previous) *previous = old;

    uint8_t next = (uint8_t)((old & (uint8_t)~mask) | (value & mask));
    if (next != old) {
        st = sdrn_reg_write_internal(dev, reg, &next, 1);
        if (st != SDRN_OK) return st;
    }
    sdrn_logf(
        dev, SDRN_LOG_TRACE,
        "reg 0x%04X: old=0x%02X mask=0x%02X value=0x%02X -> 0x%02X",
        reg, old, mask, value, next
    );
    return SDRN_OK;
}

sdrn_status sdrn_demod_read_internal(
    sdrn_device *dev, uint8_t page, uint8_t reg,
    uint8_t *data, size_t length
) {
    uint16_t value = (uint16_t)((uint16_t)((uint16_t)reg << 8) | (uint16_t)0x20u);
    uint16_t index = (uint16_t)(SDRN_CMD_DEMOD_RD | page);
    return sdrn_ctrl_read(dev, value, index, data, length);
}

sdrn_status sdrn_demod_write_internal(
    sdrn_device *dev, uint8_t page, uint8_t reg,
    const uint8_t *data, size_t length
) {
    uint16_t value = (uint16_t)((uint16_t)((uint16_t)reg << 8) | (uint16_t)0x20u);
    uint16_t index = (uint16_t)(SDRN_CMD_DEMOD_WR | page);
    return sdrn_ctrl_write(dev, value, index, data, length);
}

/* integrated I2C failure reaches host as usb STALL or EPIPE */
/* first attempt may recycle Windows control session */
/* later I2C retries stay on same replacement session */
#define SDRN_I2C_RETRY_COUNT 3u
#define SDRN_I2C_RETRY_DELAY_MS 2u

static sdrn_status i2c_ctrl_read_retry(
    sdrn_device *dev, uint16_t value, uint16_t index,
    uint8_t *data, size_t length
) {
    sdrn_status st = SDRN_ERR_USB;
    for (unsigned attempt = 0; attempt <= SDRN_I2C_RETRY_COUNT; ++attempt) {
        st = ctrl_read_impl(dev, value, index, data, length, attempt == 0u);
        if (st != SDRN_ERR_USB) return st;
        if (attempt == SDRN_I2C_RETRY_COUNT) break;
        sdrn_logf(
            dev, SDRN_LOG_DEBUG,
            "RTL2832 I2C control read retry %u/%u "
            "(value=0x%04X index=0x%04X)",
            attempt + 1u, SDRN_I2C_RETRY_COUNT, value, index
        );
        sdrn_sleep_ms(SDRN_I2C_RETRY_DELAY_MS);
    }
    return st;
}

static sdrn_status i2c_ctrl_write_retry(
    sdrn_device *dev, uint16_t value, uint16_t index,
    const uint8_t *data, size_t length
) {
    sdrn_status st = SDRN_ERR_USB;
    for (unsigned attempt = 0; attempt <= SDRN_I2C_RETRY_COUNT; ++attempt) {
        st = ctrl_write_impl(dev, value, index, data, length, attempt == 0u);
        if (st != SDRN_ERR_USB) return st;
        if (attempt == SDRN_I2C_RETRY_COUNT) break;
        sdrn_logf(
            dev, SDRN_LOG_DEBUG,
            "RTL2832 I2C control write retry %u/%u "
            "(value=0x%04X index=0x%04X data0=0x%02X len=%zu)",
            attempt + 1u, SDRN_I2C_RETRY_COUNT, value, index,
            (unsigned)data[0], length
        );
        sdrn_sleep_ms(SDRN_I2C_RETRY_DELAY_MS);
    }
    return st;
}

sdrn_status sdrn_i2c_read_reg_internal(
    sdrn_device *dev, uint8_t address, uint8_t reg,
    uint8_t *data, size_t length
) {
    uint16_t value = (uint16_t)((uint16_t)((uint16_t)reg << 8) | (uint16_t)(((uint16_t)address & (uint16_t)0x7Fu) << 1));
    return i2c_ctrl_read_retry(dev, value, SDRN_CMD_I2C_RD, data, length);
}

sdrn_status sdrn_i2c_write_reg_internal(
    sdrn_device *dev, uint8_t address, uint8_t reg,
    const uint8_t *data, size_t length
) {
    if (!dev || !data || length == 0u) return SDRN_ERR_INVALID_ARGUMENT;
    uint16_t value = (uint16_t)(
        (uint16_t)((uint16_t)reg << 8) |
        (uint16_t)(((uint16_t)address & 0x7Fu) << 1)
    );
    /* compact I2C retries EPIPE with a fixed bound */
    /* do not switch transport after tuner starts NAKing */
    return i2c_ctrl_write_retry(dev, value, SDRN_CMD_I2C_WR, data, length);
}

sdrn_status sdrn_i2c_read_direct_internal(
    sdrn_device *dev, uint8_t address, uint8_t *data, size_t length
) {
    uint16_t value = (uint16_t)((address & 0x7Fu) << 1);
    return i2c_ctrl_read_retry(dev, value, SDRN_CMD_I2C_DA_RD, data, length);
}

sdrn_status sdrn_i2c_write_direct_internal(
    sdrn_device *dev, uint8_t address, const uint8_t *data, size_t length
) {
    uint16_t value = (uint16_t)((address & 0x7Fu) << 1);
    return i2c_ctrl_write_retry(dev, value, SDRN_CMD_I2C_DA_WR, data, length);
}

sdrn_status sdrn_i2c_gate_set(sdrn_device *dev, bool open) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    uint8_t value = open ? SDRN_I2C_GATE_OPEN : SDRN_I2C_GATE_CLOSE;
    sdrn_status st = sdrn_demod_write_internal(
        dev, SDRN_I2C_GATE_PAGE, SDRN_I2C_GATE_REG, &value, 1
    );
    if (st == SDRN_OK) {
        dev->i2c_gate_open = open;
        sdrn_logf(dev, SDRN_LOG_DEBUG, "RTL2832 I2C gate %s", open ? "open" : "closed");
    }
    return st;
}

sdrn_status sdrn_ts_snapshot_state(sdrn_device *dev, sdrn_ts_snapshot *snapshot) {
    if (!dev || !snapshot) return SDRN_ERR_INVALID_ARGUMENT;
    memset(snapshot, 0, sizeof(*snapshot));
    sdrn_status st = sdrn_reg_read_internal(dev, SDRN_USB_SYSCTL_0, &snapshot->sysctl0, 1);
    if (st != SDRN_OK) return st;
    st = sdrn_reg_read_internal(dev, SDRN_USB_EPA_MAXPKT, snapshot->maxpkt, 4);
    if (st != SDRN_OK) return st;
    st = sdrn_reg_read_internal(dev, SDRN_USB_EPA_FIFO_CFG, snapshot->fifo_cfg, 4);
    if (st != SDRN_OK) return st;
    snapshot->valid = true;
    return SDRN_OK;
}

sdrn_status sdrn_ts_configure(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st = sdrn_ts_snapshot_state(dev, &dev->ts_snapshot);
    if (st != SDRN_OK) return st;

    uint8_t sysctl = (uint8_t)(dev->ts_snapshot.sysctl0 | 0x09u);
    static const uint8_t maxpkt[4] = {0x00, 0x02, 0x00, 0x00};
    static const uint8_t fifo[4] = {0x14, 0x00, 0x00, 0x00};

    st = sdrn_reg_write_internal(dev, SDRN_USB_SYSCTL_0, &sysctl, 1);
    if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_MAXPKT, maxpkt, 4);
    if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_FIFO_CFG, fifo, 4);
    if (st != SDRN_OK) goto fail;

    sdrn_logf(dev, SDRN_LOG_DEBUG, "MPEG-TS endpoint configured");
    return SDRN_OK;

fail:
    /* restore endpoint snapshot after partial reconfiguration failure */
    (void)sdrn_ts_restore(dev);
    return st;
}

sdrn_status sdrn_ts_restore(sdrn_device *dev) {
    if (!dev || !dev->ts_snapshot.valid) return SDRN_OK;
    sdrn_status first = SDRN_OK;
    sdrn_status st;

    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_FIFO_CFG, dev->ts_snapshot.fifo_cfg, 4);
    if (first == SDRN_OK && st != SDRN_OK) first = st;
    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_MAXPKT, dev->ts_snapshot.maxpkt, 4);
    if (first == SDRN_OK && st != SDRN_OK) first = st;
    st = sdrn_reg_write_internal(dev, SDRN_USB_SYSCTL_0, &dev->ts_snapshot.sysctl0, 1);
    if (first == SDRN_OK && st != SDRN_OK) first = st;

    dev->ts_snapshot.valid = false;
    return first;
}

static sdrn_status power_snapshot(sdrn_device *dev, sdrn_power_snapshot *s) {
    memset(s, 0, sizeof(*s));
    sdrn_status st;
    st = sdrn_reg_read_internal(dev, SDRN_SYS_GPIO_OUT_VAL, &s->gpio_out, 1); if (st != SDRN_OK) return st;
    st = sdrn_reg_read_internal(dev, SDRN_SYS_GPIO_DIR, &s->gpio_dir, 1); if (st != SDRN_OK) return st;
    st = sdrn_reg_read_internal(dev, SDRN_SYS_GPIO_OUT_EN, &s->gpio_out_en, 1); if (st != SDRN_OK) return st;
    st = sdrn_reg_read_internal(dev, SDRN_SYS_DEMOD_CTL1, &s->demod_ctl1, 1); if (st != SDRN_OK) return st;
    st = sdrn_reg_read_internal(dev, SDRN_SYS_DEMOD_CTL, &s->demod_ctl, 1); if (st != SDRN_OK) return st;
    st = sdrn_reg_read_internal(dev, SDRN_USB_EPA_CTL, s->epa_ctl, 2); if (st != SDRN_OK) return st;
    s->valid = true;
    return SDRN_OK;
}

sdrn_status sdrn_power_on_dvb(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st = power_snapshot(dev, &dev->power_snapshot);
    if (st != SDRN_OK) return st;

    st = sdrn_reg_write_mask_internal(dev, SDRN_SYS_GPIO_OUT_VAL, 0x08, 0x18, NULL); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_mask_internal(dev, SDRN_SYS_DEMOD_CTL1, 0x00, 0x10, NULL); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_mask_internal(dev, SDRN_SYS_DEMOD_CTL, 0x80, 0x80, NULL); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_mask_internal(dev, SDRN_SYS_DEMOD_CTL, 0x20, 0x20, NULL); if (st != SDRN_OK) goto fail;
    sdrn_sleep_ms(5);
    static const uint8_t epa_clear[2] = {0x00, 0x00};
    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_CTL, epa_clear, 2); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_mask_internal(dev, SDRN_SYS_DEMOD_CTL, 0x48, 0x48, NULL); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_mask_internal(dev, SDRN_SYS_GPIO_DIR, 0x00, 0x40, NULL); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_mask_internal(dev, SDRN_SYS_GPIO_OUT_EN, 0x48, 0x48, NULL); if (st != SDRN_OK) goto fail;

    sdrn_logf(dev, SDRN_LOG_DEBUG, "RTL2832 DVB hardware enabled");
    return SDRN_OK;

fail:
    /* restore pre mode GPIO and endpoint snapshot on failure */
    (void)sdrn_power_restore(dev);
    return st;
}


sdrn_status sdrn_iq_usb_power_on(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;

    sdrn_status st = power_snapshot(dev, &dev->power_snapshot);
    if (st != SDRN_OK) return st;
    st = sdrn_ts_snapshot_state(dev, &dev->ts_snapshot);
    if (st != SDRN_OK) {
        dev->power_snapshot.valid = false;
        return st;
    }

    /* baseband setup writes two byte values in RTL28xx vendor byte order */
    static const uint8_t sysctl = 0x09u;
    static const uint8_t maxpkt[2] = {0x00u, 0x02u};
    static const uint8_t epa_ctl[2] = {0x10u, 0x02u};
    static const uint8_t demod_ctl1 = 0x22u;
    static const uint8_t demod_ctl = 0xE8u;

    st = sdrn_reg_write_internal(dev, SDRN_USB_SYSCTL_0, &sysctl, 1); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_MAXPKT, maxpkt, 2); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_CTL, epa_ctl, 2); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_internal(dev, SDRN_SYS_DEMOD_CTL1, &demod_ctl1, 1); if (st != SDRN_OK) goto fail;
    st = sdrn_reg_write_internal(dev, SDRN_SYS_DEMOD_CTL, &demod_ctl, 1); if (st != SDRN_OK) goto fail;

    sdrn_logf(dev, SDRN_LOG_DEBUG, "RTL2832 IQ baseband enabled");
    return SDRN_OK;

fail:
    (void)sdrn_ts_restore(dev);
    (void)sdrn_power_restore(dev);
    return st;
}

sdrn_status sdrn_stream_quiesce_endpoint(sdrn_device *dev) {
    if (!dev || !dev->usb) return SDRN_ERR_INVALID_ARGUMENT;

    /* stream and control paths have separate transport lifetimes */
    /* normalize host pipe before frontend restore */
    /* keep endpoint A reset until sdrn_power_restore() restores EPA_CTL */
    sdrn_status first = SDRN_OK;

    if (dev->usb && dev->usb->clear_halt) {
        char err[256] = {0};
        sdrn_status st = dev->usb->clear_halt(
            dev->usb_ctx, dev->usb_handle, SDRN_TS_ENDPOINT, err, sizeof(err)
        );
        if (st != SDRN_OK) {
            first = st;
            uint32_t flags = SDRN_ERROR_FLAG_NONE;
            if (st == SDRN_ERR_NOT_FOUND) {
                dev->device_gone = true;
                flags |= SDRN_ERROR_FLAG_DEVICE_GONE;
            }
            sdrn_set_error_infof(
                dev, st, SDRN_ERROR_DOMAIN_USB, sdrn_usb_native_error(dev),
                flags,
                "bulk endpoint 0x81 reset before stop failed: %s",
                err[0] ? err : sdrn_status_string(st)
            );
            /* NO_DEVICE means endpoint reset cannot touch old handle */
            if (st == SDRN_ERR_NOT_FOUND) return st;
        }
    }

    static const uint8_t reset[2] = {0x10u, 0x02u};
    sdrn_status reset_st = sdrn_reg_write_internal(
        dev, SDRN_USB_EPA_CTL, reset, 2u
    );
    if (first == SDRN_OK && reset_st != SDRN_OK) first = reset_st;

    if (first == SDRN_OK) {
        dev->stream_transport_armed = false;
        sdrn_logf(
            dev, SDRN_LOG_DEBUG,
            "stream stopped; endpoint/FIFO held in reset"
        );
    }
    return first;
}

sdrn_status sdrn_stream_reset_endpoint(sdrn_device *dev) {
    if (!dev || !dev->usb) return SDRN_ERR_INVALID_ARGUMENT;

    /* 0x1002 resets endpoint A and FIFO */
    /* 0x0000 releases bulk IN traffic */
    static const uint8_t reset[2] = {0x10u, 0x02u};
    static const uint8_t release[2] = {0x00u, 0x00u};

    sdrn_status st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_CTL, reset, 2);
    if (st != SDRN_OK) return st;
    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_CTL, release, 2);
    if (st != SDRN_OK) return st;

    /* clear host halt after releasing endpoint 0x81 */
    /* mode changes must not leak stalled pipe state */
    if (dev->usb && dev->usb->clear_halt) {
        char err[256] = {0};
        st = dev->usb->clear_halt(
            dev->usb_ctx, dev->usb_handle, SDRN_TS_ENDPOINT, err, sizeof(err)
        );
        if (st != SDRN_OK) {
            sdrn_set_error_infof(
                dev, st, SDRN_ERROR_DOMAIN_USB, sdrn_usb_native_error(dev),
                SDRN_ERROR_FLAG_NONE,
                "bulk endpoint 0x81 clear-halt failed: %s",
                err[0] ? err : sdrn_status_string(st)
            );
            return st;
        }
    }

    dev->stream_transport_armed = false;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "bulk endpoint/FIFO reset; halt cleared"
    );
    return SDRN_OK;
}

sdrn_status sdrn_power_restore(sdrn_device *dev) {
    if (!dev || !dev->power_snapshot.valid) return SDRN_OK;
    sdrn_status first = SDRN_OK;
    sdrn_status st;
    sdrn_power_snapshot *s = &dev->power_snapshot;

    st = sdrn_reg_write_internal(dev, SDRN_USB_EPA_CTL, s->epa_ctl, 2); if (first == SDRN_OK && st != SDRN_OK) first = st;
    st = sdrn_reg_write_internal(dev, SDRN_SYS_DEMOD_CTL, &s->demod_ctl, 1); if (first == SDRN_OK && st != SDRN_OK) first = st;
    st = sdrn_reg_write_internal(dev, SDRN_SYS_DEMOD_CTL1, &s->demod_ctl1, 1); if (first == SDRN_OK && st != SDRN_OK) first = st;
    st = sdrn_reg_write_internal(dev, SDRN_SYS_GPIO_OUT_EN, &s->gpio_out_en, 1); if (first == SDRN_OK && st != SDRN_OK) first = st;
    st = sdrn_reg_write_internal(dev, SDRN_SYS_GPIO_DIR, &s->gpio_dir, 1); if (first == SDRN_OK && st != SDRN_OK) first = st;
    st = sdrn_reg_write_internal(dev, SDRN_SYS_GPIO_OUT_VAL, &s->gpio_out, 1); if (first == SDRN_OK && st != SDRN_OK) first = st;
    s->valid = false;
    return first;
}

/* advanced api */
sdrn_status sdrn_control_read(
    sdrn_device *device,
    uint16_t value,
    uint16_t index,
    void *data,
    size_t length
) {
    if (!device || !data || length == 0u) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_ctrl_read(device, value, index, (uint8_t *)data, length);
}

sdrn_status sdrn_control_write(
    sdrn_device *device,
    uint16_t value,
    uint16_t index,
    const void *data,
    size_t length
) {
    if (!device || !data || length == 0u) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_ctrl_write(device, value, index, (const uint8_t *)data, length);
}

sdrn_status sdrn_reg_read(sdrn_device *device, uint16_t reg, void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_reg_read_internal(device, reg, (uint8_t *)data, length);
}

sdrn_status sdrn_reg_write(sdrn_device *device, uint16_t reg, const void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_reg_write_internal(device, reg, (const uint8_t *)data, length);
}

sdrn_status sdrn_demod_read(sdrn_device *device, uint8_t page, uint8_t reg, void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_demod_read_internal(device, page, reg, (uint8_t *)data, length);
}

sdrn_status sdrn_demod_write(sdrn_device *device, uint8_t page, uint8_t reg, const void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_demod_write_internal(device, page, reg, (const uint8_t *)data, length);
}

sdrn_status sdrn_i2c_read_reg(sdrn_device *device, uint8_t address, uint8_t reg, void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_i2c_read_reg_internal(device, address, reg, (uint8_t *)data, length);
}

sdrn_status sdrn_i2c_write_reg(sdrn_device *device, uint8_t address, uint8_t reg, const void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_i2c_write_reg_internal(device, address, reg, (const uint8_t *)data, length);
}

sdrn_status sdrn_i2c_read_direct(sdrn_device *device, uint8_t address, void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_i2c_read_direct_internal(device, address, (uint8_t *)data, length);
}

sdrn_status sdrn_i2c_write_direct(sdrn_device *device, uint8_t address, const void *data, size_t length) {
    if (!device || !data) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_i2c_write_direct_internal(device, address, (const uint8_t *)data, length);
}

sdrn_status sdrn_i2c_gate_open(sdrn_device *device) {
    return sdrn_i2c_gate_set(device, true);
}

sdrn_status sdrn_i2c_gate_close(sdrn_device *device) {
    return sdrn_i2c_gate_set(device, false);
}