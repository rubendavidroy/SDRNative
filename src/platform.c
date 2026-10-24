/*
 * SDRNative
 * Platform helpers
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "internal.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

void sdrn_sleep_ms(uint32_t milliseconds) {
#if defined(_WIN32)
    /* hardware settle delays are minimum durations */
    /* Win32 Sleep can return early below system clock resolution */
    /* verify elapsed time with QueryPerformanceCounter */
    if (milliseconds == 0u) {
        Sleep(0);
        return;
    }

    LARGE_INTEGER frequency, start, now;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&start) || frequency.QuadPart <= 0) {
        /* extra millisecond keeps fallback above requested minimum */
        Sleep((DWORD)milliseconds + 1u);
        return;
    }

    const uint64_t target_ticks =
        (((uint64_t)frequency.QuadPart * (uint64_t)milliseconds) + 999u) / 1000u;

    Sleep((DWORD)milliseconds);
    for (;;) {
        if (!QueryPerformanceCounter(&now)) break;
        uint64_t elapsed = (uint64_t)(now.QuadPart - start.QuadPart);
        if (elapsed >= target_ticks) break;
        Sleep(0);
    }
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(milliseconds / 1000u);
    ts.tv_nsec = (long)((milliseconds % 1000u) * 1000000u);
    while (nanosleep(&ts, &ts) != 0) {
        /* retry interrupted sleep */
    }
#endif
}

uint64_t sdrn_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency, now;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&now) || frequency.QuadPart <= 0) {
        return (uint64_t)GetTickCount64();
    }
    const uint64_t ticks = (uint64_t)now.QuadPart;
    const uint64_t hz = (uint64_t)frequency.QuadPart;
    return (ticks / hz) * 1000ull + ((ticks % hz) * 1000ull) / hz;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ull) + ((uint64_t)ts.tv_nsec / 1000000ull);
#endif
}