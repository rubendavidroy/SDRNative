/*
 * SDRNative
 * RTL2832 DVB T frontend
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#include <math.h>
#include <string.h>

typedef struct field {
    uint8_t page;
    uint8_t reg;
    uint8_t msb;
    uint8_t lsb;
} field;

typedef struct field_value {
    field f;
    uint32_t value;
} field_value;

#define FIELD(p,r,m,l) {(p),(r),(m),(l)}

static const field F_SOFT_RST = FIELD(1, 0x01, 2, 2);
static const field F_EN_BBIN = FIELD(1, 0xB1, 0, 0);
static const field F_PSET_IFFREQ = FIELD(1, 0x19, 21, 0);
static const field F_RSAMP_RATIO = FIELD(1, 0x9F, 27, 2);
static const field F_CFREQ_OFF_RATIO = FIELD(1, 0x9D, 23, 4);
static const field F_FSM_STAGE = FIELD(3, 0x51, 6, 3);

static const field_value INITIAL_REGS[] = {
    {FIELD(0,0x08,7,7), 0x1}, {FIELD(0,0x08,6,6), 0x1},
    {FIELD(1,0x8F,15,0), 0x2800},
    {FIELD(1,0x95,7,0), 0x10}, {FIELD(1,0x96,7,0), 0x20},
    {FIELD(1,0x97,7,0), 0x20}, {FIELD(1,0x98,7,0), 0x40},
    {FIELD(1,0x99,7,0), 0x22}, {FIELD(1,0x9A,7,0), 0x32},
    {FIELD(1,0x9B,7,0), 0x37}, {FIELD(1,0x9C,7,0), 0x39},
    {FIELD(1,0xA6,7,7), 0x0}, {FIELD(1,0x61,4,4), 0x0},
    {FIELD(0,0x09,6,0), 0x2A}, {FIELD(0,0x0A,2,0), 0x6},
    {FIELD(0,0x21,3,3), 0x0},
    {FIELD(1,0x7D,3,0), 0x8}, {FIELD(1,0x7D,7,4), 0x8},
    {FIELD(2,0x92,7,0), 0x4}, {FIELD(2,0x93,7,0), 0xB0},
    {FIELD(2,0xA7,7,0), 0x78}, {FIELD(2,0xA9,7,0), 0x28},
    {FIELD(2,0xAA,7,0), 0x59}, {FIELD(2,0xAB,7,0), 0x83},
    {FIELD(2,0xAC,7,0), 0xD4}, {FIELD(2,0xB0,7,0), 0x65},
    {FIELD(2,0xB1,7,0), 0x43},
    {FIELD(1,0x64,3,1), 0x1}, {FIELD(1,0x64,6,4), 0x4},
    {FIELD(1,0x65,2,0), 0x7}, {FIELD(2,0xAD,9,4), 0xA},
    {FIELD(0,0x0D,7,7), 0x1}, {FIELD(1,0x7C,4,4), 0x0},
    {FIELD(1,0x7D,3,0), 0x9}, {FIELD(1,0x7D,7,4), 0x9},
    {FIELD(0,0x06,7,7), 0x0}, {FIELD(0,0x07,7,6), 0x0},
    {FIELD(1,0x6F,2,0), 0x4}, {FIELD(1,0x70,5,3), 0x7},
    {FIELD(1,0x72,3,0), 0x6}, {FIELD(1,0x75,2,0), 0x5},
    {FIELD(1,0x76,7,6), 0x1},
};

static const field_value R820T_DEMOD_REGS[] = {
    {FIELD(1,0x12,7,0), 0x39},
    {FIELD(1,0x02,0,0), 0x0},
    {FIELD(1,0x03,7,0), 0x40},
    {FIELD(1,0xC7,5,1), 0x16},
    {FIELD(1,0x04,4,1), 0x8},
    {FIELD(1,0x05,7,7), 0x1},
    {FIELD(1,0xC8,4,0), 0x18},
    {FIELD(1,0x06,5,0), 0x35}, {FIELD(1,0xC9,5,0), 0x21},
    {FIELD(1,0xCA,5,0), 0x21},
    {FIELD(1,0xCB,7,0), 0x0}, {FIELD(1,0x07,7,0), 0x40},
    {FIELD(1,0xCD,7,0), 0x10}, {FIELD(1,0xCE,7,0), 0x10},
    {FIELD(1,0x08,7,0), 0x80}, {FIELD(1,0x09,7,0), 0x7F},
    {FIELD(1,0x0A,7,0), 0x80}, {FIELD(1,0x0B,7,0), 0x7F},
    {FIELD(0,0x0E,1,1), 0x0}, {FIELD(0,0x0E,0,0), 0x0},
    {FIELD(0,0x11,15,0), 0xE9F4},
    {FIELD(1,0x15,0,0), 0x1},
};

static const uint8_t BW_6[32] = {
    0xF5,0xFF,0x15,0x38,0x5D,0x6D,0x52,0x07,
    0xFA,0x2F,0x53,0xF5,0x3F,0xCA,0x0B,0x91,
    0xEA,0x30,0x63,0xB2,0x13,0xDA,0x0B,0xC4,
    0x18,0x7E,0x16,0x66,0x08,0x67,0x19,0xE0,
};
static const uint8_t BW_7[32] = {
    0xE7,0xCC,0xB5,0xBA,0xE8,0x2F,0x67,0x61,
    0x00,0xAF,0x86,0xF2,0xBF,0x59,0x04,0x11,
    0xB6,0x33,0xA4,0x30,0x15,0x10,0x0A,0x42,
    0x18,0xF8,0x17,0xD9,0x07,0x22,0x19,0x10,
};
static const uint8_t BW_8[32] = {
    0x09,0xF6,0xD2,0xA7,0x9A,0xC9,0x27,0x77,
    0x06,0xBF,0xEC,0xF4,0x4F,0x0B,0xFC,0x01,
    0x63,0x35,0x54,0xA7,0x16,0x66,0x08,0xB4,
    0x19,0x6E,0x19,0x65,0x05,0xC8,0x19,0xE0,
};

static const uint32_t SNR_CONSTANTS[3][4] = {
    {85387325u,85387325u,85387325u,85387325u},
    {86676178u,86676178u,87167949u,87795660u},
    {87659938u,87659938u,87885178u,88241743u},
};

static uint32_t mask_width(unsigned width) {
    if (width >= 32u) return 0xFFFFFFFFu;
    return (1u << width) - 1u;
}

static uint32_t be_read(const uint8_t *data, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | data[i];
    return v;
}

static void be_write(uint32_t value, uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        data[n - 1 - i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

static sdrn_status field_read(sdrn_device *dev, field f, uint32_t *out) {
    if (!out) return SDRN_ERR_INVALID_ARGUMENT;
    size_t length = (size_t)(f.msb >> 3) + 1u;
    uint8_t raw[4] = {0};
    if (length > sizeof(raw)) return SDRN_ERR_INTERNAL;
    sdrn_status st = sdrn_demod_read_internal(dev, f.page, f.reg, raw, length);
    if (st != SDRN_OK) return st;
    uint32_t width = (uint32_t)f.msb - (uint32_t)f.lsb + 1u;
    *out = (be_read(raw, length) >> f.lsb) & mask_width(width);
    return SDRN_OK;
}

static sdrn_status field_write(sdrn_device *dev, field f, uint32_t value) {
    size_t length = (size_t)(f.msb >> 3) + 1u;
    uint8_t raw[4] = {0};
    if (length > sizeof(raw)) return SDRN_ERR_INTERNAL;
    sdrn_status st = sdrn_demod_read_internal(dev, f.page, f.reg, raw, length);
    if (st != SDRN_OK) return st;
    uint32_t current = be_read(raw, length);
    uint32_t width = (uint32_t)f.msb - (uint32_t)f.lsb + 1u;
    uint32_t mask = mask_width(width);
    uint32_t updated = current & ~(mask << f.lsb);
    updated |= (value & mask) << f.lsb;
    be_write(updated, raw, length);
    return sdrn_demod_write_internal(dev, f.page, f.reg, raw, length);
}

static sdrn_status write_field_table(
    sdrn_device *dev,
    const field_value *table,
    size_t count
) {
    for (size_t i = 0; i < count; ++i) {
        sdrn_status st = field_write(dev, table[i].f, table[i].value);
        if (st != SDRN_OK) return st;
    }
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_attach(sdrn_device *dev) {
    uint8_t chip = 0;
    sdrn_status st = sdrn_demod_read_internal(dev, 0, 0x00, &chip, 1);
    if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "RTL2832 frontend: page 0 reg 0x00=0x%02X", chip);
    return field_write(dev, F_SOFT_RST, 1u);
}

static sdrn_status unset_sdr_mode(sdrn_device *dev) {
    static const uint8_t a[] = {0xE0};
    static const uint8_t b[] = {0x20};
    static const uint8_t c[] = {0x11,0x10};
    static const uint8_t d[] = {0x00,0x0F,0xFF};
    static const uint8_t e[] = {0x40,0x00};
    static const uint8_t f[] = {0x06,0x3F,0xCE,0xCC};
    sdrn_status st;
    st = sdrn_demod_write_internal(dev,0,0x61,a,sizeof(a));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_write_internal(dev,0,0x19,b,sizeof(b));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_write_internal(dev,0,0x17,c,sizeof(c));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_write_internal(dev,1,0x92,d,sizeof(d));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_write_internal(dev,1,0x3E,e,sizeof(e));
    if (st != SDRN_OK) return st;
    st = sdrn_demod_write_internal(dev,1,0x15,f,sizeof(f));
    if (st != SDRN_OK) return st;
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_initialize_r820t(sdrn_device *dev) {
    sdrn_status st = unset_sdr_mode(dev);
    if (st != SDRN_OK) return st;
    st = field_write(dev, F_SOFT_RST, 0u);
    if (st != SDRN_OK) return st;

    st = write_field_table(dev, INITIAL_REGS, sizeof(INITIAL_REGS)/sizeof(INITIAL_REGS[0]));
    if (st != SDRN_OK) return st;
    st = write_field_table(dev, R820T_DEMOD_REGS, sizeof(R820T_DEMOD_REGS)/sizeof(R820T_DEMOD_REGS[0]));
    if (st != SDRN_OK) return st;

    st = field_write(dev, F_SOFT_RST, 1u); if (st != SDRN_OK) return st;
    st = field_write(dev, F_SOFT_RST, 0u); if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "RTL2832 DVB-T frontend initialized");
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_configure_bandwidth(
    sdrn_device *dev, uint32_t bandwidth_hz, uint32_t if_hz
) {
    const uint8_t *params = NULL;
    uint32_t bw_mode = 0;
    switch (bandwidth_hz) {
        case 6000000u: params = BW_6; bw_mode = 48000000u; break;
        case 7000000u: params = BW_7; bw_mode = 56000000u; break;
        case 8000000u: params = BW_8; bw_mode = 64000000u; break;
        default:
            sdrn_set_error_infof(dev, SDRN_ERR_UNSUPPORTED, SDRN_ERROR_DOMAIN_DEMOD, 0, SDRN_ERROR_FLAG_NONE, "unsupported DVB-T bandwidth %u Hz", bandwidth_hz);
            return SDRN_ERR_UNSUPPORTED;
    }

    uint32_t en_bbin = if_hz == 0u ? 1u : 0u;
    uint64_t pset = if_hz % SDRN_XTAL_HZ;
    pset = (pset * 0x400000ull) / SDRN_XTAL_HZ;
    pset = ((uint64_t)(-(int64_t)pset)) & 0x3FFFFFu;

    sdrn_status st = field_write(dev, F_EN_BBIN, en_bbin);
    if (st != SDRN_OK) return st;
    st = field_write(dev, F_PSET_IFFREQ, (uint32_t)pset);
    if (st != SDRN_OK) return st;

    for (uint8_t i = 0; i < 32u; ++i) {
        st = sdrn_demod_write_internal(dev, 1, (uint8_t)(0x1C + i), &params[i], 1);
        if (st != SDRN_OK) return st;
    }

    uint64_t resamp64 = ((uint64_t)SDRN_XTAL_HZ * 7ull * 0x400000ull) / bw_mode;
    uint32_t resamp = (uint32_t)(resamp64 & 0x3FFFFFFu);
    st = field_write(dev, F_RSAMP_RATIO, resamp);
    if (st != SDRN_OK) return st;

    uint64_t cf64 = ((uint64_t)bw_mode << 20) / ((uint64_t)SDRN_XTAL_HZ * 7ull);
    uint32_t cfreq = (uint32_t)((-(int64_t)cf64) & 0xFFFFFu);
    st = field_write(dev, F_CFREQ_OFF_RATIO, cfreq);
    if (st != SDRN_OK) return st;

    st = field_write(dev, F_SOFT_RST, 1u); if (st != SDRN_OK) return st;
    st = field_write(dev, F_SOFT_RST, 0u); if (st != SDRN_OK) return st;

    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "RTL2832 DVB-T: BW=%u MHz IF=%.3f MHz PSET=0x%06X RSAMP=0x%07X CFREQ=0x%05X",
        (unsigned)(bandwidth_hz / 1000000u),
        (double)if_hz / 1e6,
        (unsigned)pset,
        resamp,
        cfreq
    );
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_set_pid_filter(
    sdrn_device *dev, const uint16_t *pids, size_t count
) {
    if (!dev || !pids || count == 0 || count > SDRN_MAX_PID_FILTER) {
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    uint16_t unique[SDRN_MAX_PID_FILTER];
    size_t unique_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (pids[i] > 0x1FFFu) {
            sdrn_set_error_infof(dev, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0, SDRN_ERROR_FLAG_NONE, "invalid MPEG-TS PID 0x%04X", pids[i]);
            return SDRN_ERR_INVALID_ARGUMENT;
        }
        bool seen = false;
        for (size_t j = 0; j < unique_count; ++j) {
            if (unique[j] == pids[i]) { seen = true; break; }
        }
        if (!seen) unique[unique_count++] = pids[i];
    }

    uint8_t raw = 0;
    sdrn_status st = sdrn_demod_read_internal(dev, 0, 0x61, &raw, 1);
    if (st != SDRN_OK) return st;
    uint8_t disabled = (uint8_t)((raw & ~0xC0u) | 0xC0u);
    st = sdrn_demod_write_internal(dev, 0, 0x61, &disabled, 1);
    if (st != SDRN_OK) return st;

    for (size_t i = 0; i < unique_count; ++i) {
        uint8_t pid_bytes[2] = {
            (uint8_t)((unique[i] >> 8) & 0x1Fu),
            (uint8_t)(unique[i] & 0xFFu)
        };
        st = sdrn_demod_write_internal(
            dev, 0, (uint8_t)(0x66u + (2u * i)), pid_bytes, 2
        );
        if (st != SDRN_OK) return st;
    }

    uint32_t bitmap = unique_count == 32u ? 0xFFFFFFFFu : ((1u << unique_count) - 1u);
    uint8_t bitmap_le[4] = {
        (uint8_t)(bitmap & 0xFFu),
        (uint8_t)((bitmap >> 8) & 0xFFu),
        (uint8_t)((bitmap >> 16) & 0xFFu),
        (uint8_t)((bitmap >> 24) & 0xFFu)
    };
    st = sdrn_demod_write_internal(dev, 0, 0x62, bitmap_le, 4);
    if (st != SDRN_OK) return st;

    uint8_t enabled = (uint8_t)((raw & ~0xC0u) | 0x80u);
    st = sdrn_demod_write_internal(dev, 0, 0x61, &enabled, 1);
    if (st != SDRN_OK) return st;
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_disable_pid_filter(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    uint8_t raw = 0;
    sdrn_status st = sdrn_demod_read_internal(dev, 0, 0x61, &raw, 1);
    if (st != SDRN_OK) return st;
    uint8_t disabled = (uint8_t)((raw & ~0xC0u) | 0xC0u);
    return sdrn_demod_write_internal(dev, 0, 0x61, &disabled, 1);
}

sdrn_status sdrn_rtl2832_get_fsm(sdrn_device *dev, uint8_t *out_stage) {
    if (!dev || !out_stage) return SDRN_ERR_INVALID_ARGUMENT;
    uint32_t v = 0;
    sdrn_status st = field_read(dev, F_FSM_STAGE, &v);
    if (st != SDRN_OK) return st;
    *out_stage = (uint8_t)v;
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_get_quality(
    sdrn_device *dev, sdrn_signal_quality *out_quality
) {
    if (!dev || !out_quality) return SDRN_ERR_INVALID_ARGUMENT;
    memset(out_quality, 0, sizeof(*out_quality));

    sdrn_status st = sdrn_rtl2832_get_fsm(dev, &out_quality->fsm_stage);
    if (st != SDRN_OK) return st;

    if (out_quality->fsm_stage == 10u || out_quality->fsm_stage == 11u) {
        uint8_t dagc = 0;
        st = sdrn_demod_read_internal(dev, 3, 0x05, &dagc, 1);
        if (st != SDRN_OK) return st;
        uint8_t strength_u8 = (uint8_t)~dagc;
        int pct = (int)lround((100.0 * strength_u8) / 255.0);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        out_quality->strength_percent = pct;
    }

    uint8_t cfg = 0;
    st = sdrn_demod_read_internal(dev, 3, 0x3C, &cfg, 1);
    if (st != SDRN_OK) return st;
    unsigned constellation = (cfg >> 2) & 0x03u;
    unsigned hierarchy = (cfg >> 4) & 0x07u;
    if (constellation < 3u && hierarchy < 4u) {
        uint8_t evm_raw[2] = {0};
        st = sdrn_demod_read_internal(dev, 4, 0x0C, evm_raw, 2);
        if (st != SDRN_OK) return st;
        uint32_t evm = ((uint32_t)evm_raw[0] << 8) | evm_raw[1];
        if (evm > 0u) {
            double constant_log10 = (double)SNR_CONSTANTS[constellation][hierarchy] / (double)(1u << 24);
            double tenth_db = 100.0 * (constant_log10 - log10((double)evm));
            out_quality->snr_db = tenth_db / 10.0;
            out_quality->snr_valid = 1u;
        }
    }

    uint8_t ber_raw[2] = {0};
    st = sdrn_demod_read_internal(dev, 3, 0x4E, ber_raw, 2);
    if (st != SDRN_OK) return st;
    out_quality->ber = ((uint32_t)ber_raw[0] << 8) | ber_raw[1];
    /* 0xFFFF is RTL2832 BER saturation value */
    /* preserve raw value but mark it invalid */
    out_quality->ber_valid = out_quality->ber != 0xFFFFu ? 1u : 0u;
    return SDRN_OK;
}

sdrn_status sdrn_rtl2832_release(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    return field_write(dev, F_SOFT_RST, 1u);
}