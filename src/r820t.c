/*
 * SDRNative
 * R820T tuner support
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#include <string.h>

#define R820T_REG_START 0x05u
#define R820T_REG_END   0x1Fu
#define R820T_VER_NUM   49u
#define R820T_VCO_POWER_REF 2u
#define R820T_LT_SETTLE_MS 5u
#define R820T_FILTER_TRIGGER_SETTLE_MS 5u

static const uint8_t INIT_REGS[] = {
    0x83,0x32,0x75,
    0xC0,0x40,0xD6,0x6C,
    0xF5,0x63,0x75,0x68,
    0x6C,0x83,0x80,0x00,
    0x0F,0x00,0xC0,0x30,
    0x48,0xCC,0x60,0x00,
    0x54,0xAE,0x4A,0xC0,
};

typedef struct freq_range {
    uint16_t freq_mhz;
    uint8_t open_d;
    uint8_t rf_mux_ploy;
    uint8_t tf_c;
    uint8_t xtal_cap0p;
} freq_range;

static const freq_range FREQ_RANGES[] = {
    {0,   0x08,0x02,0xDF,0x00},
    {50,  0x08,0x02,0xBE,0x00},
    {55,  0x08,0x02,0x8B,0x00},
    {60,  0x08,0x02,0x7B,0x00},
    {65,  0x08,0x02,0x69,0x00},
    {70,  0x08,0x02,0x58,0x00},
    {75,  0x00,0x02,0x44,0x00},
    {80,  0x00,0x02,0x44,0x00},
    {90,  0x00,0x02,0x34,0x00},
    {100, 0x00,0x02,0x34,0x00},
    {110, 0x00,0x02,0x24,0x00},
    {120, 0x00,0x02,0x24,0x00},
    {140, 0x00,0x02,0x14,0x00},
    {180, 0x00,0x02,0x13,0x00},
    {220, 0x00,0x02,0x13,0x00},
    {250, 0x00,0x02,0x11,0x00},
    {280, 0x00,0x02,0x00,0x00},
    {310, 0x00,0x41,0x00,0x00},
    {450, 0x00,0x41,0x00,0x00},
    {588, 0x00,0x40,0x00,0x00},
    {650, 0x00,0x40,0x00,0x00},
};

static uint8_t bit_reverse8(uint8_t x) {
    x = (uint8_t)(((x & 0xF0u) >> 4) | ((x & 0x0Fu) << 4));
    x = (uint8_t)(((x & 0xCCu) >> 2) | ((x & 0x33u) << 2));
    x = (uint8_t)(((x & 0xAAu) >> 1) | ((x & 0x55u) << 1));
    return x;
}

void sdrn_r820t_reset_state(sdrn_device *dev) {
    if (!dev) return;
    memset(&dev->tuner, 0, sizeof(dev->tuner));
    memcpy(dev->tuner.regs, INIT_REGS, sizeof(INIT_REGS));
}

static size_t cache_index(uint8_t reg) {
    return (size_t)(reg - R820T_REG_START);
}

static uint8_t cache_get(const sdrn_device *dev, uint8_t reg) {
    return dev->tuner.regs[cache_index(reg)];
}

static void cache_set(sdrn_device *dev, uint8_t reg, uint8_t value) {
    dev->tuner.regs[cache_index(reg)] = value;
}

static sdrn_status tuner_i2c_write_reg(
    sdrn_device *dev, uint8_t reg, const uint8_t *data, size_t length
) {
    if (!dev || !data || length == 0u) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_i2c_write_reg_internal(dev, SDRN_R820T_ADDR, reg, data, length);
}

static sdrn_status tuner_i2c_read_reg(
    sdrn_device *dev, uint8_t reg, uint8_t *data, size_t length
) {
    if (!dev || !data || length == 0u) return SDRN_ERR_INVALID_ARGUMENT;
    return sdrn_i2c_read_reg_internal(dev, SDRN_R820T_ADDR, reg, data, length);
}

static sdrn_status tuner_write_reg_force(sdrn_device *dev, uint8_t reg, uint8_t value) {
    sdrn_status st = tuner_i2c_write_reg(dev, reg, &value, 1u);
    if (st == SDRN_OK && reg >= R820T_REG_START && reg <= R820T_REG_END) {
        cache_set(dev, reg, value);
    }
    return st;
}

static sdrn_status tuner_write_reg(sdrn_device *dev, uint8_t reg, uint8_t value) {
    if (reg >= R820T_REG_START && reg <= R820T_REG_END && cache_get(dev, reg) == value) {
        sdrn_logf(
            dev, SDRN_LOG_TRACE,
            "R820T reg 0x%02X unchanged (0x%02X)",
            reg, value
        );
        return SDRN_OK;
    }
    return tuner_write_reg_force(dev, reg, value);
}

static sdrn_status tuner_write_mask(
    sdrn_device *dev, uint8_t reg, uint8_t value, uint8_t mask
) {
    if (reg < R820T_REG_START || reg > R820T_REG_END) {
        sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T reg 0x%02X missing from shadow", reg);
        return SDRN_ERR_TUNER;
    }
    uint8_t old = cache_get(dev, reg);
    uint8_t next = (uint8_t)((old & (uint8_t)~mask) | (value & mask));
    if (next == old) {
        sdrn_logf(
            dev, SDRN_LOG_TRACE,
            "R820T masked reg 0x%02X unchanged (0x%02X)",
            reg, next
        );
        return SDRN_OK;
    }
    return tuner_write_reg_force(dev, reg, next);
}

static sdrn_status tuner_write_init(sdrn_device *dev) {
    /* tuner shadow only changes after successful hardware write */
    /* start mismatched so init table can never be skipped */
    memset(dev->tuner.regs, 0, sizeof(dev->tuner.regs));

    for (size_t i = 0; i < sizeof(INIT_REGS); ++i) {
        sdrn_status st = tuner_write_reg_force(
            dev, (uint8_t)(R820T_REG_START + i), INIT_REGS[i]
        );
        if (st != SDRN_OK) return st;
    }
    return SDRN_OK;
}

static sdrn_status tuner_read_status(
    sdrn_device *dev, uint8_t *raw, uint8_t *reversed, size_t length
) {
    if (length == 0 || length > 5 || !raw || !reversed) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st = tuner_i2c_read_reg(dev, 0x00, raw, length);
    if (st != SDRN_OK) return st;
    for (size_t i = 0; i < length; ++i) reversed[i] = bit_reverse8(raw[i]);
    return SDRN_OK;
}

sdrn_status sdrn_r820t_probe(sdrn_device *dev, uint8_t *out_id) {
    if (!dev || !out_id) return SDRN_ERR_INVALID_ARGUMENT;
    uint8_t id = 0;
    sdrn_status st = tuner_i2c_read_reg(dev, 0x00, &id, 1);
    if (st != SDRN_OK) return st;
    *out_id = id;
    if (id != SDRN_R820T_ID) {
        sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "unsupported tuner ID 0x%02X at I2C 0x1A (expected 0x69)", id);
        return SDRN_ERR_TUNER;
    }
    sdrn_logf(dev, SDRN_LOG_INFO, "R820T-family tuner detected (ID 0x69)");
    return SDRN_OK;
}

sdrn_status sdrn_r820t_initialize_fast(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st = tuner_write_init(dev);
    if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x0C, 0x00, 0x0F); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x13, R820T_VER_NUM, 0x3F); if (st != SDRN_OK) return st;
    dev->tuner.initialized = true;
    dev->tuner.imr_done = false;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "R820T initialized (no IMR calibration)");
    return SDRN_OK;
}

/* dvb setup writes base table then enters TV standard config once */
/* iq uses separate fast init path */
sdrn_status sdrn_r820t_prepare_dvbt(sdrn_device *dev) {
    if (!dev) return SDRN_ERR_INVALID_ARGUMENT;
    sdrn_status st = tuner_write_init(dev);
    if (st != SDRN_OK) return st;
    dev->tuner.initialized = true;
    dev->tuner.imr_done = false;
    dev->tuner.bandwidth_hz = 0u;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T DVB tuner initialized"
    );
    return SDRN_OK;
}

static const freq_range *select_range(uint64_t freq_hz) {
    double mhz = (double)freq_hz / 1e6;
    const freq_range *selected = &FREQ_RANGES[(sizeof(FREQ_RANGES)/sizeof(FREQ_RANGES[0])) - 1];
    for (size_t i = 0; i + 1 < sizeof(FREQ_RANGES)/sizeof(FREQ_RANGES[0]); ++i) {
        if (mhz < FREQ_RANGES[i + 1].freq_mhz) {
            selected = &FREQ_RANGES[i];
            break;
        }
    }
    return selected;
}

static sdrn_status set_mux(sdrn_device *dev, uint64_t freq_hz) {
    const freq_range *r = select_range(freq_hz);
    sdrn_status st;
    st = tuner_write_mask(dev, 0x17, r->open_d, 0x08); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x1A, r->rf_mux_ploy, 0xC3); if (st != SDRN_OK) return st;
    st = tuner_write_reg(dev, 0x1B, r->tf_c); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x10, r->xtal_cap0p, 0x0B); if (st != SDRN_OK) return st;
    /* fast mode uses zero IMR gain and phase correction */
    st = tuner_write_mask(dev, 0x08, 0x00, 0x3F); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x09, 0x00, 0x3F); if (st != SDRN_OK) return st;
    return SDRN_OK;
}

static sdrn_status set_pll(sdrn_device *dev, uint64_t freq_hz, bool *out_locked) {
    if (!out_locked) return SDRN_ERR_INVALID_ARGUMENT;
    *out_locked = false;

    uint64_t freq_khz = freq_hz / 1000ull;
    uint64_t pll_ref_khz = SDRN_XTAL_HZ / 1000u;
    sdrn_status st;

    st = tuner_write_mask(dev, 0x10, 0x00, 0x10); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x1A, 0x00, 0x0C); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x12, 0x80, 0xE0); if (st != SDRN_OK) return st;

    uint32_t mix_div = 2u;
    int div_num = 0;
    const uint64_t vco_min = 1770000ull;
    const uint64_t vco_max = vco_min * 2ull;
    bool found = false;
    while (mix_div <= 64u) {
        uint64_t vco = freq_khz * mix_div;
        if (vco >= vco_min && vco < vco_max) {
            uint32_t div_buf = mix_div;
            while (div_buf > 2u) {
                div_buf >>= 1;
                ++div_num;
            }
            found = true;
            break;
        }
        mix_div <<= 1;
    }
    if (!found) {
        sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T: no mixer divider for %.3f MHz", (double)freq_hz / 1e6);
        return SDRN_ERR_TUNER;
    }

    uint8_t raw5[5] = {0}, data5[5] = {0};
    st = tuner_read_status(dev, raw5, data5, 5); if (st != SDRN_OK) return st;
    uint8_t fine = (uint8_t)((data5[4] & 0x30u) >> 4);
    if (fine > R820T_VCO_POWER_REF) --div_num;
    else if (fine < R820T_VCO_POWER_REF) ++div_num;
    if (div_num < 0 || div_num > 7) {
        sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T: invalid PLL divider %d", div_num);
        return SDRN_ERR_TUNER;
    }
    st = tuner_write_mask(dev, 0x10, (uint8_t)(div_num << 5), 0xE0); if (st != SDRN_OK) return st;

    uint64_t vco_freq = freq_khz * mix_div;
    uint64_t nint = vco_freq / (2ull * pll_ref_khz);
    uint64_t vco_fra = vco_freq - (2ull * pll_ref_khz * nint);

    if (vco_fra < pll_ref_khz / 64ull) {
        vco_fra = 0;
    } else if (vco_fra > (pll_ref_khz * 127ull) / 64ull) {
        vco_fra = 0;
        ++nint;
    } else if (((pll_ref_khz * 127ull) / 128ull) < vco_fra && vco_fra < pll_ref_khz) {
        vco_fra = (pll_ref_khz * 127ull) / 128ull;
    } else if (pll_ref_khz < vco_fra && vco_fra < ((pll_ref_khz * 129ull) / 128ull)) {
        vco_fra = (pll_ref_khz * 129ull) / 128ull;
    }

    if (nint < 13ull) {
        sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T PLL NINT below minimum: %llu", (unsigned long long)nint);
        return SDRN_ERR_TUNER;
    }

    uint64_t ni = (nint - 13ull) / 4ull;
    uint64_t si = nint - (4ull * ni) - 13ull;
    st = tuner_write_reg(dev, 0x14, (uint8_t)(ni + (si << 6))); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x12, vco_fra == 0 ? 0x08 : 0x00, 0x08); if (st != SDRN_OK) return st;

    uint32_t n_sdm = 2u;
    uint32_t sdm = 0u;
    uint64_t remain = vco_fra;
    while (remain > 1ull) {
        uint64_t step = (2ull * pll_ref_khz) / n_sdm;
        if (step == 0ull) break;
        if (remain > step) {
            sdm += 32768u / (n_sdm / 2u);
            remain -= step;
            if (n_sdm >= 0x8000u) break;
        }
        n_sdm <<= 1;
    }

    st = tuner_write_reg(dev, 0x16, (uint8_t)((sdm >> 8) & 0xFFu)); if (st != SDRN_OK) return st;
    st = tuner_write_reg(dev, 0x15, (uint8_t)(sdm & 0xFFu)); if (st != SDRN_OK) return st;

    for (int attempt = 0; attempt < 2; ++attempt) {
        sdrn_sleep_ms(10);
        uint8_t raw3[3] = {0}, data3[3] = {0};
        st = tuner_read_status(dev, raw3, data3, 3); if (st != SDRN_OK) return st;
        if (data3[2] & 0x40u) {
            *out_locked = true;
            break;
        }
        if (attempt == 0) {
            st = tuner_write_mask(dev, 0x12, 0x60, 0xE0); if (st != SDRN_OK) return st;
        }
    }

    dev->tuner.has_lock = *out_locked;
    if (*out_locked) {
        st = tuner_write_mask(dev, 0x1A, 0x08, 0x08);
        if (st != SDRN_OK) return st;
    }
    return SDRN_OK;
}

static sdrn_status configure_dvbt(sdrn_device *dev, uint32_t bandwidth_hz) {
    if (!dev->tuner.initialized) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "R820T not initialized"
        );
        return SDRN_ERR_STATE;
    }

    uint32_t if_hz;
    uint64_t filter_lo;
    uint8_t hp_cor;
    if (bandwidth_hz == 6000000u) {
        if_hz = 3570000u; filter_lo = 56000000ull; hp_cor = 0x6B;
    } else if (bandwidth_hz == 7000000u) {
        if_hz = 4570000u; filter_lo = 63000000ull; hp_cor = 0x2A;
    } else if (bandwidth_hz == 8000000u) {
        if_hz = 4570000u; filter_lo = 68500000ull; hp_cor = 0x0B;
    } else {
        sdrn_set_error_infof(dev, SDRN_ERR_UNSUPPORTED, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T DVB-T bandwidth must be 6, 7 or 8 MHz");
        return SDRN_ERR_UNSUPPORTED;
    }

    memcpy(dev->tuner.regs, INIT_REGS, sizeof(INIT_REGS));
    sdrn_status st;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T DVB setup: reg 0x0C -> 0x13 -> 0x1D"
    );
    st = tuner_write_mask(dev, 0x0C, dev->tuner.imr_done ? 0x09 : 0x00, 0x0F); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x13, R820T_VER_NUM, 0x3F); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x1D, 0x00, 0x38); if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "DVB tuner setup complete");

    /* 0x1D LT gain transition needs a real minimum settle */
    /* keep 1 ms delay before filter calibration */
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T filter settling >=%u ms before reg 0x0B",
        R820T_LT_SETTLE_MS
    );
    uint64_t settle_started_ms = sdrn_now_ms();
    sdrn_sleep_ms(R820T_LT_SETTLE_MS);
    uint64_t settle_elapsed_ms = sdrn_now_ms() - settle_started_ms;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T filter settled (%llu ms); reg 0x0B=0x%02X",
        (unsigned long long)settle_elapsed_ms,
        (unsigned int)((INIT_REGS[0x0Bu - R820T_REG_START] & 0x9Fu) | (hp_cor & 0x60u))
    );

    dev->tuner.if_hz = if_hz;
    dev->tuner.bandwidth_hz = bandwidth_hz;

    for (int attempt = 0; attempt < 2; ++attempt) {
        st = tuner_write_mask(dev, 0x0B, hp_cor, 0x60); if (st != SDRN_OK) return st;
        st = tuner_write_mask(dev, 0x0F, 0x04, 0x04); if (st != SDRN_OK) return st;
        st = tuner_write_mask(dev, 0x10, 0x00, 0x03); if (st != SDRN_OK) return st;

        bool lock = false;
        st = set_pll(dev, filter_lo, &lock); if (st != SDRN_OK) return st;
        if (!lock) {
            sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T calibration PLL did not lock");
            return SDRN_ERR_TUNER;
        }

        st = tuner_write_mask(dev, 0x0B, 0x10, 0x10); if (st != SDRN_OK) return st;
        /* keep calibration trigger settled for at least 1 ms */
        sdrn_sleep_ms(R820T_FILTER_TRIGGER_SETTLE_MS);
        st = tuner_write_mask(dev, 0x0B, 0x00, 0x10); if (st != SDRN_OK) return st;
        st = tuner_write_mask(dev, 0x0F, 0x00, 0x04); if (st != SDRN_OK) return st;

        uint8_t raw[5] = {0}, rev[5] = {0};
        st = tuner_read_status(dev, raw, rev, 5); if (st != SDRN_OK) return st;
        dev->tuner.filter_cal_code = (uint8_t)(rev[4] & 0x0Fu);
        if (dev->tuner.filter_cal_code != 0x00u && dev->tuner.filter_cal_code != 0x0Fu) break;
    }
    if (dev->tuner.filter_cal_code == 0x0Fu) dev->tuner.filter_cal_code = 0;

    st = tuner_write_mask(dev, 0x0A, (uint8_t)(0x10u | dev->tuner.filter_cal_code), 0x1F); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x0B, hp_cor, 0xEF); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x07, 0x00, 0x80); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x06, 0x10, 0x30); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x1E, 0x60, 0x60); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x05, 0x00, 0x80); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x1F, 0x00, 0x80); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x0F, 0x00, 0x80); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x19, 0x60, 0x60); if (st != SDRN_OK) return st;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T DVB filter: cal=0x%X, IF=%.3f MHz",
        (unsigned)dev->tuner.filter_cal_code,
        (double)dev->tuner.if_hz / 1e6
    );
    return SDRN_OK;
}

static sdrn_status sysfreq_dvbt(sdrn_device *dev, uint64_t rf_hz) {
    uint8_t mixer_top = 0x24;
    uint8_t cp_cur = 0x38;
    uint8_t div_buf_cur = 0x30;

    /* these three dvb frequencies use alternate mixer and PLL current */
    if (rf_hz == 506000000ull || rf_hz == 666000000ull || rf_hz == 818000000ull) {
        mixer_top = 0x14;
        cp_cur = 0x28;
        div_buf_cur = 0x20;
    }

    sdrn_status st;
    uint8_t reg1d_before = cache_get(dev, 0x1D);
    uint8_t reg1d_next = (uint8_t)((reg1d_before & 0x38u) | (0xE5u & 0xC7u));
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T DVB reg 0x1D: 0x%02X -> 0x%02X (mask 0xC7)",
        reg1d_before, reg1d_next
    );
    st = tuner_write_mask(dev,0x1D,0xE5,0xC7); if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "R820T DVB reg 0x1D written");
    st = tuner_write_mask(dev,0x1C,mixer_top,0xF8); if (st != SDRN_OK) return st;
    st = tuner_write_reg(dev,0x0D,0x53); if (st != SDRN_OK) return st;
    st = tuner_write_reg(dev,0x0E,0x75); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x05,0x00,0x60); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x06,0x00,0x08); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x11,cp_cur,0x38); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x17,div_buf_cur,0x30); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x0A,0x40,0x60); if (st != SDRN_OK) return st;

    st = tuner_write_mask(dev,0x1D,0x00,0x38); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x1C,0x00,0x04); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x06,0x00,0x40); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x1A,0x30,0x30); if (st != SDRN_OK) return st;

    /* close tuner gate during 250 ms LNA settle */
    /* reopen only when next tuner write is ready */
    st = sdrn_i2c_gate_set(dev, false); if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "R820T LNA settling: I2C gate closed for 250 ms");
    sdrn_sleep_ms(250);
    st = sdrn_i2c_gate_set(dev, true); if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "R820T LNA settled; I2C gate reopened");

    st = tuner_write_mask(dev,0x1D,0x18,0x38); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x1C,mixer_top,0x04); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x1E,14,0x1F); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev,0x1A,0x20,0x30); if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "R820T DVB settings applied");
    return SDRN_OK;
}

sdrn_status sdrn_r820t_tune_dvbt(
    sdrn_device *dev,
    uint64_t rf_hz,
    uint32_t bandwidth_hz,
    sdrn_dvbt_tune_info *out_tune
) {
    if (!dev || !out_tune) return SDRN_ERR_INVALID_ARGUMENT;
    if (!dev->tuner.initialized) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "R820T not initialized"
        );
        return SDRN_ERR_STATE;
    }

    sdrn_status st;
    if (dev->tuner.bandwidth_hz != bandwidth_hz) {
        st = configure_dvbt(dev, bandwidth_hz);
        if (st != SDRN_OK) return st;
    }

    uint64_t lo_hz = rf_hz + dev->tuner.if_hz;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T RF mux: LO %.3f MHz",
        (double)lo_hz / 1e6
    );
    st = set_mux(dev, lo_hz); if (st != SDRN_OK) return st;
    sdrn_logf(dev, SDRN_LOG_DEBUG, "R820T RF mux configured");

    bool pll_lock = false;
    st = set_pll(dev, lo_hz, &pll_lock); if (st != SDRN_OK) return st;
    if (!pll_lock) {
        sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T PLL did not lock at %.3f MHz", (double)lo_hz / 1e6);
        return SDRN_ERR_TUNER;
    }
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T PLL locked: LO %.3f MHz; configuring AGC",
        (double)lo_hz / 1e6
    );

    st = sysfreq_dvbt(dev, rf_hz); if (st != SDRN_OK) return st;

    memset(out_tune, 0, sizeof(*out_tune));
    out_tune->rf_hz = rf_hz;
    out_tune->bandwidth_hz = bandwidth_hz;
    out_tune->if_hz = dev->tuner.if_hz;
    out_tune->lo_hz = lo_hz;
    out_tune->tuner_pll_locked = 1u;
    out_tune->filter_cal_code = dev->tuner.filter_cal_code;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T DVB tune: RF=%.3f MHz BW=%u MHz IF=%.3f MHz LO=%.3f MHz",
        (double)rf_hz / 1e6,
        (unsigned)(bandwidth_hz / 1000000u),
        (double)dev->tuner.if_hz / 1e6,
        (double)lo_hz / 1e6
    );
    return SDRN_OK;
}

sdrn_status sdrn_r820t_standby(sdrn_device *dev) {
    if (!dev || !dev->tuner.initialized) return SDRN_OK;
    static const uint8_t regs[][2] = {
        {0x06,0xB1},{0x05,0x03},{0x07,0x3A},{0x08,0x40},
        {0x09,0xC0},{0x0A,0x36},{0x0C,0x35},{0x0F,0x68},
        {0x11,0x03},{0x17,0xF4},{0x19,0x0C},
    };
    sdrn_status first = SDRN_OK;
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); ++i) {
        sdrn_status st = tuner_write_reg(dev, regs[i][0], regs[i][1]);
        if (first == SDRN_OK && st != SDRN_OK) first = st;
    }
    return first;
}

/* raw iq tuner path */

static const uint32_t IQ_LPF_BW[] = {
    1700000u, 1600000u, 1550000u, 1450000u, 1200000u,
    900000u, 700000u, 550000u, 450000u, 350000u
};

static sdrn_status configure_iq_bandwidth(
    sdrn_device *dev, uint32_t bandwidth_hz, uint32_t *out_if_hz
) {
    if (!dev || !out_if_hz) return SDRN_ERR_INVALID_ARGUMENT;
    if (!dev->tuner.initialized) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "R820T not initialized"
        );
        return SDRN_ERR_STATE;
    }
    if (bandwidth_hz < 225000u || bandwidth_hz > 8000000u) {
        sdrn_set_error_infof(dev, SDRN_ERR_INVALID_ARGUMENT, SDRN_ERROR_DOMAIN_ARGUMENT, 0, SDRN_ERROR_FLAG_NONE, "R820T IQ bandwidth outside 225 kHz..8 MHz");
        return SDRN_ERR_INVALID_ARGUMENT;
    }

    uint8_t reg0a;
    uint8_t reg0b;
    uint32_t if_hz;

    if (bandwidth_hz > 7000000u) {
        reg0a = 0x10u;
        reg0b = 0x0Bu;
        if_hz = 4570000u;
    } else if (bandwidth_hz > 6000000u) {
        reg0a = 0x10u;
        reg0b = 0x2Au;
        if_hz = 4570000u;
    } else if (bandwidth_hz > (1700000u + 350000u + 380000u)) {
        reg0a = 0x10u;
        reg0b = 0x6Bu;
        if_hz = 3570000u;
    } else {
        uint32_t remaining = bandwidth_hz;
        uint32_t real_bw = 0u;
        reg0a = 0x00u;
        reg0b = 0x80u;
        if_hz = 2300000u;

        if (remaining > 1700000u + 350000u) {
            remaining -= 380000u;
            if_hz += 380000u;
            real_bw += 380000u;
        } else {
            reg0b |= 0x20u;
        }

        if (remaining > 1700000u) {
            remaining -= 350000u;
            if_hz += 350000u;
            real_bw += 350000u;
        } else {
            reg0b |= 0x40u;
        }

        size_t chosen = (sizeof(IQ_LPF_BW) / sizeof(IQ_LPF_BW[0])) - 1u;
        for (size_t i = 0; i < sizeof(IQ_LPF_BW) / sizeof(IQ_LPF_BW[0]); ++i) {
            if (remaining > IQ_LPF_BW[i]) {
                chosen = (i == 0u) ? 0u : i - 1u;
                break;
            }
        }
        reg0b |= (uint8_t)(15u - (uint8_t)chosen);
        real_bw += IQ_LPF_BW[chosen];
        if_hz -= real_bw / 2u;
    }

    sdrn_status st = tuner_write_mask(dev, 0x0Au, reg0a, 0x10u);
    if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x0Bu, reg0b, 0xEFu);
    if (st != SDRN_OK) return st;

    dev->tuner.bandwidth_hz = bandwidth_hz;
    dev->tuner.if_hz = if_hz;
    *out_if_hz = if_hz;
    return SDRN_OK;
}

static sdrn_status configure_iq_auto_gain(sdrn_device *dev) {
    sdrn_status st;
    /* auto LNA and mixer with fixed VGA */
    st = tuner_write_mask(dev, 0x05u, 0x00u, 0x10u); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x07u, 0x10u, 0x10u); if (st != SDRN_OK) return st;
    st = tuner_write_mask(dev, 0x0Cu, 0x0Bu, 0x9Fu); if (st != SDRN_OK) return st;
    return SDRN_OK;
}

sdrn_status sdrn_r820t_tune_iq(
    sdrn_device *dev,
    uint64_t rf_hz,
    uint32_t bandwidth_hz,
    uint32_t *out_if_hz,
    uint64_t *out_lo_hz
) {
    if (!dev || !out_if_hz || !out_lo_hz) return SDRN_ERR_INVALID_ARGUMENT;
    if (!dev->tuner.initialized) {
        sdrn_set_error_infof(
            dev, SDRN_ERR_STATE, SDRN_ERROR_DOMAIN_STATE, 0,
            SDRN_ERROR_FLAG_NONE, "R820T not initialized"
        );
        return SDRN_ERR_STATE;
    }

    uint32_t if_hz = 0;
    sdrn_status st = configure_iq_bandwidth(dev, bandwidth_hz, &if_hz);
    if (st != SDRN_OK) return st;

    uint64_t lo_hz = rf_hz + (uint64_t)if_hz;
    st = set_mux(dev, lo_hz); if (st != SDRN_OK) return st;

    bool lock = false;
    st = set_pll(dev, lo_hz, &lock); if (st != SDRN_OK) return st;
    if (!lock) {
        sdrn_set_error_infof(dev, SDRN_ERR_TUNER, SDRN_ERROR_DOMAIN_TUNER, 0, SDRN_ERROR_FLAG_NONE, "R820T IQ PLL failed to lock at %.3f MHz", (double)lo_hz / 1e6);
        return SDRN_ERR_TUNER;
    }

    st = configure_iq_auto_gain(dev); if (st != SDRN_OK) return st;

    *out_if_hz = if_hz;
    *out_lo_hz = lo_hz;
    sdrn_logf(
        dev, SDRN_LOG_DEBUG,
        "R820T IQ tune: RF=%.3f MHz BW=%.3f MHz IF=%.3f MHz LO=%.3f MHz",
        (double)rf_hz / 1e6,
        (double)bandwidth_hz / 1e6,
        (double)if_hz / 1e6,
        (double)lo_hz / 1e6
    );
    return SDRN_OK;
}