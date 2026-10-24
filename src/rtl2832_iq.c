/*
 * SDRNative
 * RTL2832 raw IQ mode
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#include <math.h>
#include <stdint.h>

/* raw iq path ends at RTL2832 byte stream */
/* no software DSP in raw iq mode */

#define IQ_FIR_LEN 16u

static const int IQ_FIR[IQ_FIR_LEN] = {
    -54, -36, -41, -40, -32, -14, 14, 53,
    101, 156, 215, 273, 327, 372, 404, 421
};

static sdrn_status demod_write1(sdrn_device *dev, uint8_t page, uint8_t reg, uint8_t value) {
    return sdrn_demod_write_internal(dev, page, reg, &value, 1);
}

static sdrn_status demod_write_u16_be(
    sdrn_device *dev, uint8_t page, uint8_t reg, uint16_t value
) {
    uint8_t raw[2] = {
        (uint8_t)((value >> 8) & 0xFFu),
        (uint8_t)(value & 0xFFu)
    };
    return sdrn_demod_write_internal(dev, page, reg, raw, 2);
}

sdrn_status sdrn_rtl2832_iq_snapshot_state(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;

    sdrn_iq_demod_snapshot *snap = &dev->iq_demod_snapshot;
    snap->valid = false;

    sdrn_status st;
    st = sdrn_demod_read_internal(dev, 0u, 0x06u, &snap->page0_reg06, 1u);
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 0u, 0x0Du, &snap->page0_reg0d, 1u);
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 0u, 0x19u, &snap->page0_reg19, 1u);
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 0u, 0x61u, &snap->page0_reg61, 1u);
    if (st != SDRN_OK) return st;

    /* snapshot only registers changed by iq mode */
    /* keep demod transfers short across backends */
    st = sdrn_demod_read_internal(dev, 1u, 0x01u, &snap->page1_reg01, 1u);
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0x04u, &snap->page1_reg04, 1u);
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0x11u, &snap->page1_reg11, 1u);
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0x15u, snap->page1_15_1b, sizeof(snap->page1_15_1b));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0x1Cu, snap->page1_1c_2b, sizeof(snap->page1_1c_2b));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0x3Eu, snap->page1_3e_3f, sizeof(snap->page1_3e_3f));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0x93u, snap->page1_93_94, sizeof(snap->page1_93_94));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0x9Fu, snap->page1_9f_a2, sizeof(snap->page1_9f_a2));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_read_internal(dev, 1u, 0xB1u, &snap->page1_b1, 1u);
    if (st != SDRN_OK) return st;

    snap->valid = true;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "saved IQ demod registers (p1/01=0x%02X p1/04=0x%02X p1/11=0x%02X p1/B1=0x%02X)",
        snap->page1_reg01, snap->page1_reg04, snap->page1_reg11, snap->page1_b1
    );
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_iq_restore_state(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;

    sdrn_iq_demod_snapshot *snap = &dev->iq_demod_snapshot;
    if (!snap->valid) return SDRN_OK;

    sdrn_status first = SDRN_OK;
    sdrn_status st;

#define RESTORE_DEMOD(page_, reg_, data_, length_) do { \
        st = sdrn_demod_write_internal(dev, (page_), (reg_), (data_), (length_)); \
        if (first == SDRN_OK && st != SDRN_OK) first = st; \
    } while (0)

    RESTORE_DEMOD(1u, 0x15u, snap->page1_15_1b, sizeof(snap->page1_15_1b));
    RESTORE_DEMOD(1u, 0x1Cu, snap->page1_1c_2b, sizeof(snap->page1_1c_2b));
    RESTORE_DEMOD(1u, 0x3Eu, snap->page1_3e_3f, sizeof(snap->page1_3e_3f));
    RESTORE_DEMOD(1u, 0x93u, snap->page1_93_94, sizeof(snap->page1_93_94));
    RESTORE_DEMOD(1u, 0x9Fu, snap->page1_9f_a2, sizeof(snap->page1_9f_a2));
    RESTORE_DEMOD(1u, 0xB1u, &snap->page1_b1, 1u);
    RESTORE_DEMOD(1u, 0x11u, &snap->page1_reg11, 1u);
    RESTORE_DEMOD(1u, 0x04u, &snap->page1_reg04, 1u);

    RESTORE_DEMOD(0u, 0x61u, &snap->page0_reg61, 1u);
    RESTORE_DEMOD(0u, 0x19u, &snap->page0_reg19, 1u);
    RESTORE_DEMOD(0u, 0x0Du, &snap->page0_reg0d, 1u);
    RESTORE_DEMOD(0u, 0x06u, &snap->page0_reg06, 1u);

    /* restore page 1 reg 0x01 last */
    /* it owns both soft reset and I2C repeater bits */
    RESTORE_DEMOD(1u, 0x01u, &snap->page1_reg01, 1u);

#undef RESTORE_DEMOD

    if (first == SDRN_OK) {
        sdrn_logf(
            dev, SDRN_LOG_DEBUG,
            "restored IQ demod registers (p1/01=0x%02X p1/04=0x%02X p1/11=0x%02X p1/B1=0x%02X)",
            snap->page1_reg01, snap->page1_reg04, snap->page1_reg11, snap->page1_b1
        );
    }
    snap->valid = false;
    return first;
}

static sdrn_status write_iq_fir(sdrn_device *dev) {
    uint8_t fir[20];

    for (size_t i = 0; i < 8u; ++i) {
        if (IQ_FIR[i] < -128 || IQ_FIR[i] > 127) return SDRN_ERR_INTERNAL;
        fir[i] = (uint8_t)IQ_FIR[i];
    }

    for (size_t i = 0; i < 8u; i += 2u) {
        int v0 = IQ_FIR[8u + i];
        int v1 = IQ_FIR[8u + i + 1u];
        if (v0 < -2048 || v0 > 2047 || v1 < -2048 || v1 > 2047) {
            return SDRN_ERR_INTERNAL;
        }
        size_t o = 8u + (i * 3u) / 2u;
        fir[o] = (uint8_t)((v0 >> 4) & 0xFF);
        fir[o + 1u] = (uint8_t)(((v0 << 4) & 0xF0) | ((v1 >> 8) & 0x0F));
        fir[o + 2u] = (uint8_t)(v1 & 0xFF);
    }

    for (size_t i = 0; i < sizeof(fir); ++i) {
        sdrn_status st = demod_write1(dev, 1u, (uint8_t)(0x1Cu + i), fir[i]);
        if (st != SDRN_OK) return st;
    }
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_iq_initialize(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st;

    st = demod_write1(dev, 1u, 0x01u, 0x14u); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x01u, 0x10u); if (st != SDRN_OK) return st;

    /* raw iq path uses no spectral inversion or adjacent channel rejection */
    st = demod_write1(dev, 1u, 0x15u, 0x00u); if (st != SDRN_OK) return st;
    st = demod_write_u16_be(dev, 1u, 0x16u, 0x0000u); if (st != SDRN_OK) return st;

    for (uint8_t reg = 0x16u; reg <= 0x1Bu; ++reg) {
        st = demod_write1(dev, 1u, reg, 0x00u);
        if (st != SDRN_OK) return st;
    }

    st = write_iq_fir(dev); if (st != SDRN_OK) return st;

    st = demod_write1(dev, 0u, 0x19u, 0x05u); if (st != SDRN_OK) return st;

    st = demod_write1(dev, 1u, 0x93u, 0xF0u); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x94u, 0x0Fu); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x11u, 0x00u); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x04u, 0x00u); if (st != SDRN_OK) return st;

    /* pid filter has no meaning in iq mode */
    st = demod_write1(dev, 0u, 0x61u, 0x60u); if (st != SDRN_OK) return st;

    st = demod_write1(dev, 0u, 0x06u, 0x80u); if (st != SDRN_OK) return st;

    st = demod_write1(dev, 1u, 0xB1u, 0x1Bu); if (st != SDRN_OK) return st;

    st = demod_write1(dev, 0u, 0x0Du, 0x83u); if (st != SDRN_OK) return st;

    sdrn_logf(dev, SDRN_LOG_DEBUG, "RTL2832 IQ baseband initialized");
    return SDRN_OK;
}

static bool valid_iq_rate(uint32_t rate_hz) {
    if (rate_hz <= 225000u || rate_hz > 3200000u) return false;
    if (rate_hz > 300000u && rate_hz <= 900000u) return false;
    return true;
}

sdrn_status sdrn_rtl2832_iq_set_sample_rate(
    sdrn_device *dev, uint32_t requested_rate_hz, uint32_t *out_actual_rate_hz
) {
    if (!dev || !out_actual_rate_hz) return SDRN_ERR_INVALID_ARGUMENT;
    if (!valid_iq_rate(requested_rate_hz)) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0,
            SDRN_ERROR_FLAG_NONE,
            "unsupported IQ sample rate %u Hz (225k..300k or 900k..3.2M Hz)",
            requested_rate_hz
        );
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    const uint64_t scale = (1ull << 22);
    uint64_t ratio64 = ((uint64_t)SDRN_XTAL_HZ * scale) / requested_rate_hz;
    ratio64 &= 0x0FFFFFFCull;
    if (ratio64 == 0ull || ratio64 > 0x0FFFFFFFull) {
        sdrn_set_error_infof(dev, SDRN_ERR_DEMOD, SDRN_ERROR_DOMAIN_DEMOD, 0, SDRN_ERROR_FLAG_NONE, "RTL2832 IQ resampler ratio out of range");
        return SDRN_ERR_DEMOD;
    }

    uint32_t ratio = (uint32_t)ratio64;
    uint32_t real_ratio = ratio | ((ratio & 0x08000000u) << 1u);
    if (real_ratio == 0u) return SDRN_ERR_DEMOD;

    double actual = ((double)SDRN_XTAL_HZ * (double)scale) / (double)real_ratio;
    uint32_t actual_hz = (uint32_t)llround(actual);

    sdrn_status st = demod_write_u16_be(dev, 1u, 0x9Fu, (uint16_t)(ratio >> 16));
    if (st != SDRN_OK) return st;
    st = demod_write_u16_be(dev, 1u, 0xA1u, (uint16_t)(ratio & 0xFFFFu));
    if (st != SDRN_OK) return st;

    st = demod_write1(dev, 1u, 0x3Fu, 0x00u); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x3Eu, 0x00u); if (st != SDRN_OK) return st;

    /* resampler changes apply across soft reset */
    st = demod_write1(dev, 1u, 0x01u, 0x14u); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x01u, 0x10u); if (st != SDRN_OK) return st;

    *out_actual_rate_hz = actual_hz;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "RTL2832 IQ rate: requested=%u Hz actual=%u Hz ratio=0x%08X",
        requested_rate_hz, actual_hz, ratio
    );
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_iq_set_if(sdrn_device *dev, uint32_t if_hz) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;

    const int64_t scale = (1ll << 22);
    int64_t magnitude = ((int64_t)if_hz * scale) / (int64_t)SDRN_XTAL_HZ;
    int32_t if_word = (int32_t)(-magnitude);

    uint8_t b0 = (uint8_t)((if_word >> 16) & 0x3F);
    uint8_t b1 = (uint8_t)((if_word >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)(if_word & 0xFF);

    sdrn_status st = demod_write1(dev, 1u, 0x19u, b0); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x1Au, b1); if (st != SDRN_OK) return st;
    st = demod_write1(dev, 1u, 0x1Bu, b2); if (st != SDRN_OK) return st;

    sdrn_logf(dev, SDRN_LOG_DEBUG, "RTL2832 IQ IF=%.3f MHz", (double)if_hz / 1e6);
    return SDRN_OK;
}