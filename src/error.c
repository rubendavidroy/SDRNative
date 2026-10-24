/*
 * SDRNative
 * Error handling
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
__declspec(thread) static char g_last_error[512];
__declspec(thread) static sdrn_error_info g_last_error_info;
#else
static _Thread_local char g_last_error[512];
static _Thread_local sdrn_error_info g_last_error_info;
#endif

static void error_info_reset(sdrn_error_info *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->status = (int32_t)SDRN_OK;
    info->domain = (int32_t)SDRN_ERROR_DOMAIN_NONE;
}

static void error_info_vset(
    sdrn_error_info *info,
    char *text,
    size_t text_cap,
    sdrn_status status,
    sdrn_error_domain domain,
    int32_t native_code,
    uint32_t flags,
    const char *fmt,
    va_list args
) {
    char message[512] = {0};
    if (fmt) vsnprintf(message, sizeof(message), fmt, args);

    if (text && text_cap > 0u) {
        snprintf(text, text_cap, "%s", message);
    }
    if (!info) return;

    error_info_reset(info);
    info->status = (int32_t)status;
    info->domain = (int32_t)domain;
    info->native_code = native_code;
    info->flags = flags;
    size_t length = strlen(message);
    if (length >= sizeof(info->message)) length = sizeof(info->message) - 1u;
    memcpy(info->message, message, length);
    info->message[length] = '\0';
}

const char *sdrn_last_global_error(void) {
    return g_last_error;
}

sdrn_status sdrn_get_last_global_error_info(sdrn_error_info *out_info) {
    if (!out_info) return SDRN_ERR_INVALID_ARGUMENT;
    *out_info = g_last_error_info;
    return SDRN_OK;
}

void sdrn_clear_global_error(void) {
    g_last_error[0] = '\0';
    error_info_reset(&g_last_error_info);
}

void sdrn_set_global_error_infof(
    sdrn_status status,
    sdrn_error_domain domain,
    int32_t native_code,
    uint32_t flags,
    const char *fmt,
    ...
) {
    va_list args;
    va_start(args, fmt);
    error_info_vset(
        &g_last_error_info,
        g_last_error,
        sizeof(g_last_error),
        status,
        domain,
        native_code,
        flags,
        fmt,
        args
    );
    va_end(args);
}

void sdrn_set_global_errorf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    error_info_vset(
        &g_last_error_info,
        g_last_error,
        sizeof(g_last_error),
        SDRN_ERR_INTERNAL,
        SDRN_ERROR_DOMAIN_INTERNAL,
        0,
        SDRN_ERROR_FLAG_NONE,
        fmt,
        args
    );
    va_end(args);
}

#define SDRN_STR2(x) #x
#define SDRN_STR(x) SDRN_STR2(x)

const char *sdrn_version_string(void) {
    static const char release_version[] =
        SDRN_STR(SDRN_VERSION_MAJOR) "."
        SDRN_STR(SDRN_VERSION_MINOR) "."
        SDRN_STR(SDRN_VERSION_PATCH);
    static const char prerelease_version[] =
        SDRN_STR(SDRN_VERSION_MAJOR) "."
        SDRN_STR(SDRN_VERSION_MINOR) "."
        SDRN_STR(SDRN_VERSION_PATCH) "-" SDRN_VERSION_PRERELEASE;

    return SDRN_VERSION_PRERELEASE[0] != '\0' ? prerelease_version : release_version;
}

const char *sdrn_status_string(sdrn_status status) {
    switch (status) {
        case SDRN_OK: return "ok";
        case SDRN_ERR_INVALID_ARGUMENT: return "invalid argument";
        case SDRN_ERR_NO_MEMORY: return "out of memory";
        case SDRN_ERR_NOT_FOUND: return "device not found";
        case SDRN_ERR_ACCESS: return "access denied";
        case SDRN_ERR_USB: return "USB error";
        case SDRN_ERR_TIMEOUT: return "timeout";
        case SDRN_ERR_IO: return "I/O error";
        case SDRN_ERR_UNSUPPORTED: return "unsupported";
        case SDRN_ERR_STATE: return "invalid state";
        case SDRN_ERR_TUNER: return "tuner error";
        case SDRN_ERR_DEMOD: return "demodulator error";
        case SDRN_ERR_NOT_LOCKED: return "not locked";
        case SDRN_ERR_INTERNAL: return "internal error";
        default: return "unknown error";
    }
}

const char *sdrn_error_domain_string(sdrn_error_domain domain) {
    switch (domain) {
        case SDRN_ERROR_DOMAIN_NONE: return "none";
        case SDRN_ERROR_DOMAIN_ARGUMENT: return "argument";
        case SDRN_ERROR_DOMAIN_STATE: return "state";
        case SDRN_ERROR_DOMAIN_USB: return "usb";
        case SDRN_ERROR_DOMAIN_TUNER: return "tuner";
        case SDRN_ERROR_DOMAIN_DEMOD: return "demod";
        case SDRN_ERROR_DOMAIN_STREAM: return "stream";
        case SDRN_ERROR_DOMAIN_INTERNAL: return "internal";
        default: return "unknown";
    }
}

void sdrn_clear_error(sdrn_device *dev) {
    if (!dev) return;
    dev->last_error[0] = '\0';
    error_info_reset(&dev->last_error_info);
}

void sdrn_set_error_infof(
    sdrn_device *dev,
    sdrn_status status,
    sdrn_error_domain domain,
    int32_t native_code,
    uint32_t flags,
    const char *fmt,
    ...
) {
    if (!dev) return;
    va_list args;
    va_start(args, fmt);
    error_info_vset(
        &dev->last_error_info,
        dev->last_error,
        sizeof(dev->last_error),
        status,
        domain,
        native_code,
        flags,
        fmt,
        args
    );
    va_end(args);
}

void sdrn_set_errorf(sdrn_device *dev, const char *fmt, ...) {
    if (!dev) return;
    va_list args;
    va_start(args, fmt);
    error_info_vset(
        &dev->last_error_info,
        dev->last_error,
        sizeof(dev->last_error),
        SDRN_ERR_INTERNAL,
        SDRN_ERROR_DOMAIN_INTERNAL,
        0,
        SDRN_ERROR_FLAG_NONE,
        fmt,
        args
    );
    va_end(args);
}

void sdrn_logf(sdrn_device *dev, sdrn_log_level level, const char *fmt, ...) {
    if (!dev || !dev->log_fn || level > dev->log_minimum) return;

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    dev->log_fn(dev->log_user, level, buffer);
}

const char *sdrn_last_error(const sdrn_device *device) {
    if (!device) return "no device";
    return device->last_error[0] ? device->last_error : "";
}

sdrn_status sdrn_get_last_error_info(
    const sdrn_device *device,
    sdrn_error_info *out_info
) {
    if (!device || !out_info) return SDRN_ERR_INVALID_ARGUMENT;
    *out_info = device->last_error_info;
    return SDRN_OK;
}

void sdrn_set_log_callback(
    sdrn_device *device,
    sdrn_log_fn callback,
    void *user,
    sdrn_log_level minimum_level
) {
    if (!device) return;
    device->log_fn = callback;
    device->log_user = user;
    device->log_minimum = minimum_level;
}