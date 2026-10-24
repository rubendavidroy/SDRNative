/*
 * SDRNative
 * Public C API
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef SDRNATIVE_H
#define SDRNATIVE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(SDRNATIVE_SHARED)
#  if defined(SDRNATIVE_BUILD)
#    define SDRN_API __declspec(dllexport)
#  else
#    define SDRN_API __declspec(dllimport)
#  endif
#elif defined(SDRNATIVE_SHARED) && (defined(__GNUC__) || defined(__clang__))
#  define SDRN_API __attribute__((visibility("default")))
#else
#  define SDRN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SDRN_VERSION_MAJOR 0
#define SDRN_VERSION_MINOR 1
#define SDRN_VERSION_PATCH 0
#define SDRN_VERSION_PRERELEASE ""

#define SDRN_MAX_STRING 128
#define SDRN_MAX_PID_FILTER 32

/* ABI generation changes only with intentional binary break */
#define SDRN_ABI_VERSION 1u
#define SDRN_ABI_INFO_VERSION 1u
#define SDRN_ABI_FLAG_LITTLE_ENDIAN (1u << 0)
#define SDRN_ABI_FLAG_POINTER_64    (1u << 1)
#define SDRN_ABI_FLAG_ENUMS_32      (1u << 2)

#define SDRN_R820T_MIN_FREQUENCY_HZ 24000000ull
#define SDRN_R820T_MAX_FREQUENCY_HZ 1766000000ull

/* zero is success and errors are negative */
typedef enum sdrn_status {
    SDRN_OK = 0,
    SDRN_ERR_INVALID_ARGUMENT = -1,
    SDRN_ERR_NO_MEMORY = -2,
    SDRN_ERR_NOT_FOUND = -3,
    SDRN_ERR_ACCESS = -4,
    SDRN_ERR_USB = -5,
    SDRN_ERR_TIMEOUT = -6,
    SDRN_ERR_IO = -7,
    SDRN_ERR_UNSUPPORTED = -8,
    SDRN_ERR_STATE = -9,
    SDRN_ERR_TUNER = -10,
    SDRN_ERR_DEMOD = -11,
    SDRN_ERR_NOT_LOCKED = -12,
    SDRN_ERR_INTERNAL = -13
} sdrn_status;

typedef enum sdrn_log_level {
    SDRN_LOG_ERROR = 0,
    SDRN_LOG_WARN = 1,
    SDRN_LOG_INFO = 2,
    SDRN_LOG_DEBUG = 3,
    SDRN_LOG_TRACE = 4
} sdrn_log_level;

typedef enum sdrn_mode {
    SDRN_MODE_NONE = 0,
    SDRN_MODE_DVBT = 1,
    SDRN_MODE_IQ = 2
} sdrn_mode;

/* NULL maps to INVALID */
typedef enum sdrn_state {
    SDRN_STATE_INVALID = 0,
    SDRN_STATE_IDLE = 1,
    SDRN_STATE_DVBT = 2,
    SDRN_STATE_IQ = 3
} sdrn_state;

/* status stays primary machine result */
/* domain identifies source subsystem */
typedef enum sdrn_error_domain {
    SDRN_ERROR_DOMAIN_NONE = 0,
    SDRN_ERROR_DOMAIN_ARGUMENT = 1,
    SDRN_ERROR_DOMAIN_STATE = 2,
    SDRN_ERROR_DOMAIN_USB = 3,
    SDRN_ERROR_DOMAIN_TUNER = 4,
    SDRN_ERROR_DOMAIN_DEMOD = 5,
    SDRN_ERROR_DOMAIN_STREAM = 6,
    SDRN_ERROR_DOMAIN_INTERNAL = 7
} sdrn_error_domain;

#define SDRN_ERROR_INFO_MESSAGE_MAX 256
#define SDRN_ERROR_FLAG_NONE 0u
#define SDRN_ERROR_FLAG_TERMINAL_STREAM (1u << 0)
#define SDRN_ERROR_FLAG_DEVICE_GONE     (1u << 1)

/* fixed width fields keep layout stable for FFI */
/* native_code is backend specific and zero when unused */
/* libusb uses its negative error code */
/* reserved fields are zero and must be ignored */
typedef struct sdrn_error_info {
    int32_t status;
    int32_t domain;
    int32_t native_code;
    uint32_t flags;
    uint32_t reserved[4];
    char message[SDRN_ERROR_INFO_MESSAGE_MAX];
} sdrn_error_info;

typedef enum sdrn_stream_format {
    SDRN_STREAM_NONE = 0,
    SDRN_STREAM_MPEG_TS = 1,
    SDRN_STREAM_U8_IQ_INTERLEAVED = 2
} sdrn_stream_format;

typedef struct sdrn_device sdrn_device;

typedef void (*sdrn_log_fn)(void *user, sdrn_log_level level, const char *message);

typedef struct sdrn_device_info {
    uint16_t vid;
    uint16_t pid;
    uint8_t bus;
    uint8_t address;
    char manufacturer[SDRN_MAX_STRING];
    char product[SDRN_MAX_STRING];
    char serial[SDRN_MAX_STRING];
} sdrn_device_info;

typedef struct sdrn_capabilities {
    uint32_t flags;
    uint32_t dvbt_bandwidth_mask; /* bits 6 7 8 map to supported mhz */
    uint32_t max_pid_filters;
    uint32_t tuner_xtal_hz;
    uint32_t demod_xtal_hz;
} sdrn_capabilities;

#define SDRN_CAP_DVBT             (1u << 0)
#define SDRN_CAP_HW_PID_FILTER    (1u << 1)
#define SDRN_CAP_RAW_REGISTERS    (1u << 2)
#define SDRN_CAP_RAW_I2C          (1u << 3)
#define SDRN_CAP_RAW_DEMOD        (1u << 4)
#define SDRN_CAP_R820T_FAMILY     (1u << 5) /* backend identity bit */
#define SDRN_CAP_IQ               (1u << 6)
#define SDRN_CAP_GENERIC_MASK     (SDRN_CAP_DVBT | SDRN_CAP_HW_PID_FILTER | \
                                   SDRN_CAP_RAW_REGISTERS | SDRN_CAP_RAW_I2C | \
                                   SDRN_CAP_RAW_DEMOD | SDRN_CAP_IQ)

/* profile keeps generic features separate from tuner and demod identity */
/* range endpoints are inclusive */
typedef enum sdrn_tuner_family {
    SDRN_TUNER_UNKNOWN = 0,
    SDRN_TUNER_R820T_FAMILY = 1
} sdrn_tuner_family;

typedef enum sdrn_demod_family {
    SDRN_DEMOD_UNKNOWN = 0,
    SDRN_DEMOD_RTL2832 = 1
} sdrn_demod_family;

typedef struct sdrn_frequency_range {
    uint64_t min_hz;
    uint64_t max_hz;
} sdrn_frequency_range;

typedef struct sdrn_rate_range {
    uint32_t min_hz;
    uint32_t max_hz;
} sdrn_rate_range;

#define SDRN_DEVICE_PROFILE_VERSION 1u
#define SDRN_MAX_RF_RANGES 4u
#define SDRN_MAX_RATE_RANGES 4u
#define SDRN_MAX_DVBT_BANDWIDTHS 8u

typedef struct sdrn_device_profile {
    uint32_t profile_version;
    uint32_t feature_flags; /* generic SDRN_CAP bits only */
    int32_t tuner_family;
    int32_t demod_family;

    uint32_t max_pid_filters;
    uint32_t tuner_xtal_hz;
    uint32_t demod_xtal_hz;

    uint32_t rf_range_count;
    sdrn_frequency_range rf_ranges[SDRN_MAX_RF_RANGES];

    uint32_t iq_sample_rate_range_count;
    sdrn_rate_range iq_sample_rate_ranges[SDRN_MAX_RATE_RANGES];

    uint32_t iq_bandwidth_range_count;
    sdrn_rate_range iq_bandwidth_ranges[SDRN_MAX_RATE_RANGES];

    uint32_t dvbt_bandwidth_count;
    uint32_t dvbt_bandwidth_hz[SDRN_MAX_DVBT_BANDWIDTHS];

    uint32_t reserved[8];
} sdrn_device_profile;

typedef struct sdrn_dvbt_tune_info {
    uint64_t rf_hz;
    uint32_t bandwidth_hz;
    uint32_t if_hz;
    uint64_t lo_hz;
    uint8_t tuner_pll_locked;
    uint8_t filter_cal_code;
    uint8_t reserved[6];
} sdrn_dvbt_tune_info;


typedef struct sdrn_iq_tune_info {
    uint64_t rf_hz;
    uint32_t requested_sample_rate_hz;
    uint32_t actual_sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t if_hz;
    uint64_t lo_hz;
    uint8_t tuner_pll_locked;
    uint8_t reserved[7];
} sdrn_iq_tune_info;

typedef struct sdrn_stream_info {
    sdrn_stream_format format;
    uint32_t sample_rate_hz;
    uint32_t packet_size_bytes;
    uint32_t bytes_per_complex_sample;
} sdrn_stream_info;

typedef struct sdrn_signal_quality {
    uint8_t fsm_stage;
    uint8_t snr_valid;
    uint8_t ber_valid;
    uint8_t reserved0;
    int32_t strength_percent;
    double snr_db;
    uint32_t ber;
} sdrn_signal_quality;

/* per handle usb diagnostic counters */
/* counters never change recovery or hardware sequencing */
typedef struct sdrn_usb_stats {
    uint64_t control_transfers;
    uint64_t control_errors;
    uint64_t control_pipe_events;
    uint64_t session_recovery_attempts;
    uint64_t session_recovery_successes;
    uint64_t session_recovery_failures;
    uint64_t bulk_reads;
    uint64_t bulk_timeouts;
    uint64_t bulk_errors;
    uint64_t bulk_bytes;
} sdrn_usb_stats;

#define SDRN_USB_RECOVERY_STATS_VERSION 1u

/* session reopen and retried transaction are counted separately */
/* base usb stats stay unchanged for ABI generation 1 */
typedef struct sdrn_usb_recovery_stats {
    uint32_t struct_size;
    uint32_t stats_version;
    uint64_t control_pipe_events;
    uint64_t session_reopen_attempts;
    uint64_t session_reopen_successes;
    uint64_t session_reopen_failures;
    uint64_t transaction_retry_attempts;
    uint64_t transaction_retry_successes;
    uint64_t transaction_retry_failures;
    uint64_t reserved[4];
} sdrn_usb_recovery_stats;

typedef enum sdrn_async_overflow_policy {
    SDRN_ASYNC_DROP_OLDEST = 0,
    SDRN_ASYNC_DROP_NEWEST = 1
} sdrn_async_overflow_policy;

#define SDRN_ASYNC_CONFIG_VERSION 1u
#define SDRN_ASYNC_STATS_VERSION 1u

/* public async geometry limits */
/* bindings can validate config before entering native code */
#define SDRN_ASYNC_MIN_TRANSFER_SIZE (4u * 1024u)
#define SDRN_ASYNC_MAX_TRANSFER_SIZE (1024u * 1024u)
#define SDRN_ASYNC_MIN_TRANSFER_COUNT 2u
#define SDRN_ASYNC_MAX_TRANSFER_COUNT 32u
#define SDRN_ASYNC_MIN_QUEUE_DEPTH 2u
#define SDRN_ASYNC_MAX_QUEUE_DEPTH 128u
#define SDRN_ASYNC_MAX_QUEUE_BYTES (64u * 1024u * 1024u)
#define SDRN_ASYNC_MAX_TRANSFER_POOL_BYTES (32u * 1024u * 1024u)

#define SDRN_ASYNC_DEFAULT_TRANSFER_SIZE (64u * 1024u)
#define SDRN_ASYNC_DEFAULT_TRANSFER_COUNT 8u
#define SDRN_ASYNC_DEFAULT_QUEUE_DEPTH 16u

/* infinite consumer wait sentinel */
#define SDRN_ASYNC_WAIT_INFINITE UINT32_MAX

/* zero sizing fields use defaults above */
/* transfer_timeout_ms zero disables usb transfer timeout */
/* consumer timeout belongs to sdrn_async_read() */
typedef struct sdrn_async_config {
    uint32_t struct_size;
    uint32_t config_version;
    uint32_t transfer_size_bytes;
    uint32_t transfer_count;
    uint32_t queue_depth;
    uint32_t transfer_timeout_ms;
    int32_t overflow_policy;
    uint32_t reserved[9];
} sdrn_async_config;

/* bounded async queue counters */
/* consumer reads copy into caller owned buffers */
typedef struct sdrn_async_stats {
    uint32_t struct_size;
    uint32_t stats_version;
    uint32_t running;
    uint32_t queue_current;
    uint32_t queue_high_water;
    uint32_t reserved0;
    uint64_t transfer_callbacks;
    uint64_t transfers_completed;
    uint64_t transfer_bytes;
    uint64_t transfer_timeouts;
    uint64_t transfer_errors;
    uint64_t buffers_enqueued;
    uint64_t buffers_dequeued;
    uint64_t buffers_dropped_oldest;
    uint64_t buffers_dropped_newest;
    uint64_t consumer_reads;
    uint64_t consumer_bytes;
    uint64_t buffers_discarded_on_stop;
    int32_t terminal_status;
    int32_t terminal_native_code;
    uint64_t reserved[4];
} sdrn_async_stats;

/* runtime ABI layout for FFI validation */
/* sizes and alignments are bytes */
/* reserved fields are zero */
typedef struct sdrn_abi_info {
    uint32_t struct_size;
    uint32_t info_version;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t pointer_size;

    uint32_t status_size;
    uint32_t log_level_size;
    uint32_t mode_size;
    uint32_t state_size;
    uint32_t error_domain_size;
    uint32_t stream_format_size;
    uint32_t tuner_family_size;
    uint32_t demod_family_size;

    uint32_t device_info_size;
    uint32_t device_info_align;
    uint32_t capabilities_size;
    uint32_t capabilities_align;
    uint32_t error_info_size;
    uint32_t error_info_align;
    uint32_t frequency_range_size;
    uint32_t frequency_range_align;
    uint32_t rate_range_size;
    uint32_t rate_range_align;
    uint32_t device_profile_size;
    uint32_t device_profile_align;
    uint32_t dvbt_tune_info_size;
    uint32_t dvbt_tune_info_align;
    uint32_t iq_tune_info_size;
    uint32_t iq_tune_info_align;
    uint32_t stream_info_size;
    uint32_t stream_info_align;
    uint32_t signal_quality_size;
    uint32_t signal_quality_align;
    uint32_t usb_stats_size;
    uint32_t usb_stats_align;

    uint32_t reserved[8];
} sdrn_abi_info;

/* version ABI and errors */
SDRN_API const char *sdrn_version_string(void);
SDRN_API uint32_t sdrn_abi_version(void);
SDRN_API sdrn_status sdrn_get_abi_info(sdrn_abi_info *out_info);
SDRN_API const char *sdrn_status_string(sdrn_status status);
SDRN_API const char *sdrn_error_domain_string(sdrn_error_domain domain);
/* error detail for failures before device handle exists */
SDRN_API const char *sdrn_last_global_error(void);
SDRN_API sdrn_status sdrn_get_last_global_error_info(sdrn_error_info *out_info);

/* discovery and lifecycle */
/* infos NULL or capacity zero returns count only */
/* count always receives total matching devices */
SDRN_API sdrn_status sdrn_list_devices(
    sdrn_device_info *infos,
    size_t capacity,
    size_t *count
);

SDRN_API sdrn_status sdrn_open(size_t index, sdrn_device **out_device);
SDRN_API void sdrn_close(sdrn_device *device);

SDRN_API sdrn_status sdrn_get_device_info(
    const sdrn_device *device,
    sdrn_device_info *out_info
);

SDRN_API sdrn_status sdrn_get_capabilities(
    const sdrn_device *device,
    sdrn_capabilities *out_caps
);

/* profile reports backend identity separately from generic features */
/* ranges match values accepted by named APIs */
SDRN_API sdrn_status sdrn_get_device_profile(
    const sdrn_device *device,
    sdrn_device_profile *out_profile
);
SDRN_API const char *sdrn_tuner_family_string(sdrn_tuner_family family);
SDRN_API const char *sdrn_demod_family_string(sdrn_demod_family family);

SDRN_API sdrn_mode sdrn_get_mode(const sdrn_device *device);
SDRN_API sdrn_state sdrn_get_state(const sdrn_device *device);
SDRN_API const char *sdrn_last_error(const sdrn_device *device);
SDRN_API sdrn_status sdrn_get_last_error_info(
    const sdrn_device *device,
    sdrn_error_info *out_info
);
SDRN_API void sdrn_set_log_callback(
    sdrn_device *device,
    sdrn_log_fn callback,
    void *user,
    sdrn_log_level minimum_level
);

/* stop native mode and restore pre mode state */
SDRN_API sdrn_status sdrn_stop(sdrn_device *device);

/* describe current native byte stream */
SDRN_API sdrn_status sdrn_get_stream_info(
    const sdrn_device *device,
    sdrn_stream_info *out_info
);

/* usb counter reset is safe in any open state */
/* no hardware access occurs */
SDRN_API sdrn_status sdrn_get_usb_stats(
    const sdrn_device *device,
    sdrn_usb_stats *out_stats
);
SDRN_API sdrn_status sdrn_get_usb_recovery_stats(
    const sdrn_device *device,
    sdrn_usb_recovery_stats *out_stats
);
SDRN_API sdrn_status sdrn_reset_usb_stats(sdrn_device *device);

/* asynchronous streaming */
/* one async engine and one consumer per device */
/* another thread may interrupt blocked read with async stop or device stop */
/* other same handle operations remain caller synchronized */
/* sync reads and control operations are rejected while async stream is active */
/* sequence id belongs to producer buffer */
/* partial reads keep same sequence id */
/* sequence wraps naturally from UINT64_MAX to zero */
SDRN_API void sdrn_async_config_init(sdrn_async_config *config);
SDRN_API sdrn_status sdrn_async_start(
    sdrn_device *device,
    const sdrn_async_config *config
);
SDRN_API sdrn_status sdrn_async_read(
    sdrn_device *device,
    void *buffer,
    size_t capacity,
    size_t *out_read,
    uint64_t *out_sequence,
    uint32_t timeout_ms
);
SDRN_API sdrn_status sdrn_async_stop(sdrn_device *device);
SDRN_API int sdrn_async_is_running(const sdrn_device *device);
SDRN_API sdrn_status sdrn_async_get_stats(
    const sdrn_device *device,
    sdrn_async_stats *out_stats
);
SDRN_API sdrn_status sdrn_async_reset_stats(sdrn_device *device);

/* RTL2832 raw iq mode */
/* output is unsigned interleaved 8 bit I Q */
/* no software demodulation is performed */
/* bandwidth_hz zero follows sample rate */
SDRN_API sdrn_status sdrn_iq_start(
    sdrn_device *device,
    uint64_t frequency_hz,
    uint32_t sample_rate_hz,
    uint32_t bandwidth_hz,
    sdrn_iq_tune_info *out_tune
);

SDRN_API sdrn_status sdrn_iq_tune(
    sdrn_device *device,
    uint64_t frequency_hz,
    sdrn_iq_tune_info *out_tune
);

SDRN_API sdrn_status sdrn_iq_set_sample_rate(
    sdrn_device *device,
    uint32_t sample_rate_hz,
    uint32_t *out_actual_sample_rate_hz
);

/* retune R820T channel filter while iq mode stays active */
/* RTL2832 IF follows selected tuner IF */
SDRN_API sdrn_status sdrn_iq_set_bandwidth(
    sdrn_device *device,
    uint32_t bandwidth_hz,
    sdrn_iq_tune_info *out_tune
);

/* native dvb mode */
/* configures RTL2832 dvb frontend and R820T family tuner */
/* supports 6 7 and 8 mhz dvb bandwidth */
/* endpoint 0x81 returns MPEG TS */
/* no broadcast plan range is imposed */
/* service parsing and media decode stay outside SDRNative */
SDRN_API sdrn_status sdrn_dvbt_start(
    sdrn_device *device,
    uint64_t frequency_hz,
    uint32_t bandwidth_hz,
    sdrn_dvbt_tune_info *out_tune
);

/* retune active dvb mode */
SDRN_API sdrn_status sdrn_dvbt_tune(
    sdrn_device *device,
    uint64_t frequency_hz,
    uint32_t bandwidth_hz,
    sdrn_dvbt_tune_info *out_tune
);

SDRN_API sdrn_status sdrn_dvbt_wait_lock(
    sdrn_device *device,
    uint32_t timeout_ms,
    uint8_t *out_fsm_stage
);

SDRN_API sdrn_status sdrn_dvbt_get_signal_quality(
    sdrn_device *device,
    sdrn_signal_quality *out_quality
);

/* dvb returns MPEG TS and iq returns unsigned interleaved 8 bit I Q */
/* normal timeout returns SDRN_OK with zero bytes */
/* sync read is unavailable while async stream is active */
SDRN_API sdrn_status sdrn_stream_read(
    sdrn_device *device,
    void *buffer,
    size_t capacity,
    size_t *out_read,
    uint32_t timeout_ms
);

/* RTL2832 main TS pid filter supports up to 32 pids */
SDRN_API sdrn_status sdrn_dvbt_set_pid_filter(
    sdrn_device *device,
    const uint16_t *pids,
    size_t count
);
SDRN_API sdrn_status sdrn_dvbt_disable_pid_filter(sdrn_device *device);

/* advanced hardware access */
/* raw RTL28xx vendor control path */
/* value and index pass directly to device request */
SDRN_API sdrn_status sdrn_control_read(
    sdrn_device *device,
    uint16_t value,
    uint16_t index,
    void *data,
    size_t length
);
SDRN_API sdrn_status sdrn_control_write(
    sdrn_device *device,
    uint16_t value,
    uint16_t index,
    const void *data,
    size_t length
);

/* RTL28xx map uses USB below 0x3000 SYS below 0x4000 and IR above */
SDRN_API sdrn_status sdrn_reg_read(
    sdrn_device *device,
    uint16_t reg,
    void *data,
    size_t length
);
SDRN_API sdrn_status sdrn_reg_write(
    sdrn_device *device,
    uint16_t reg,
    const void *data,
    size_t length
);

/* RTL2832 demod page and register access */
SDRN_API sdrn_status sdrn_demod_read(
    sdrn_device *device,
    uint8_t page,
    uint8_t reg,
    void *data,
    size_t length
);
SDRN_API sdrn_status sdrn_demod_write(
    sdrn_device *device,
    uint8_t page,
    uint8_t reg,
    const void *data,
    size_t length
);

/* RTL28xx register addressed I2C */
SDRN_API sdrn_status sdrn_i2c_read_reg(
    sdrn_device *device,
    uint8_t address_7bit,
    uint8_t reg,
    void *data,
    size_t length
);
SDRN_API sdrn_status sdrn_i2c_write_reg(
    sdrn_device *device,
    uint8_t address_7bit,
    uint8_t reg,
    const void *data,
    size_t length
);

/* RTL28xx direct address I2C */
SDRN_API sdrn_status sdrn_i2c_read_direct(
    sdrn_device *device,
    uint8_t address_7bit,
    void *data,
    size_t length
);
SDRN_API sdrn_status sdrn_i2c_write_direct(
    sdrn_device *device,
    uint8_t address_7bit,
    const void *data,
    size_t length
);

SDRN_API sdrn_status sdrn_i2c_gate_open(sdrn_device *device);
SDRN_API sdrn_status sdrn_i2c_gate_close(sdrn_device *device);

#ifdef __cplusplus
}
#endif

#endif /* SDRNATIVE_H */