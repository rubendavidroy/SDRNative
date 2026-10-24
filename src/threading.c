/*
 * SDRNative
 * Threading helpers
 *
 * Copyright (c) 2026 Ruben Roy
 * SPDX-License-Identifier: MPL-2.0
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "internal.h"

#include <stdlib.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <process.h>

struct sdrn_thread {
    HANDLE handle;
};
struct sdrn_mutex {
    CRITICAL_SECTION cs;
};
struct sdrn_cond {
    CONDITION_VARIABLE cv;
};

typedef struct sdrn_thread_start_ctx {
    sdrn_thread_fn fn;
    void *user;
} sdrn_thread_start_ctx;

static unsigned __stdcall sdrn_thread_entry(void *opaque) {
    sdrn_thread_start_ctx *ctx = (sdrn_thread_start_ctx *)opaque;
    sdrn_thread_fn fn = ctx->fn;
    void *user = ctx->user;
    free(ctx);
    return (unsigned)fn(user);
}

sdrn_status sdrn_thread_create(sdrn_thread **out_thread, sdrn_thread_fn fn, void *user) {
    if (!out_thread || !fn) return SDRN_ERR_INVALID_ARGUMENT;
    *out_thread = NULL;
    sdrn_thread *thread = (sdrn_thread *)calloc(1, sizeof(*thread));
    sdrn_thread_start_ctx *ctx = (sdrn_thread_start_ctx *)calloc(1, sizeof(*ctx));
    if (!thread || !ctx) {
        free(thread);
        free(ctx);
        return SDRN_ERR_NO_MEMORY;
    }
    ctx->fn = fn;
    ctx->user = user;
    uintptr_t h = _beginthreadex(NULL, 0u, sdrn_thread_entry, ctx, 0u, NULL);
    if (h == 0u) {
        free(ctx);
        free(thread);
        return SDRN_ERR_INTERNAL;
    }
    thread->handle = (HANDLE)h;
    *out_thread = thread;
    return SDRN_OK;
}

void sdrn_thread_join_destroy(sdrn_thread *thread) {
    if (!thread) return;
    if (thread->handle) {
        (void)WaitForSingleObject(thread->handle, INFINITE);
        CloseHandle(thread->handle);
    }
    free(thread);
}

sdrn_status sdrn_mutex_create(sdrn_mutex **out_mutex) {
    if (!out_mutex) return SDRN_ERR_INVALID_ARGUMENT;
    *out_mutex = NULL;
    sdrn_mutex *mutex = (sdrn_mutex *)calloc(1, sizeof(*mutex));
    if (!mutex) return SDRN_ERR_NO_MEMORY;
    InitializeCriticalSection(&mutex->cs);
    *out_mutex = mutex;
    return SDRN_OK;
}

void sdrn_mutex_destroy(sdrn_mutex *mutex) {
    if (!mutex) return;
    DeleteCriticalSection(&mutex->cs);
    free(mutex);
}

void sdrn_mutex_lock(sdrn_mutex *mutex) {
    if (mutex) EnterCriticalSection(&mutex->cs);
}

void sdrn_mutex_unlock(sdrn_mutex *mutex) {
    if (mutex) LeaveCriticalSection(&mutex->cs);
}

sdrn_status sdrn_cond_create(sdrn_cond **out_cond) {
    if (!out_cond) return SDRN_ERR_INVALID_ARGUMENT;
    *out_cond = NULL;
    sdrn_cond *cond = (sdrn_cond *)calloc(1, sizeof(*cond));
    if (!cond) return SDRN_ERR_NO_MEMORY;
    InitializeConditionVariable(&cond->cv);
    *out_cond = cond;
    return SDRN_OK;
}

void sdrn_cond_destroy(sdrn_cond *cond) {
    free(cond);
}

void sdrn_cond_signal(sdrn_cond *cond) {
    if (cond) WakeConditionVariable(&cond->cv);
}

void sdrn_cond_broadcast(sdrn_cond *cond) {
    if (cond) WakeAllConditionVariable(&cond->cv);
}

int sdrn_cond_wait_ms(sdrn_cond *cond, sdrn_mutex *mutex, uint32_t timeout_ms) {
    if (!cond || !mutex) return -1;
    DWORD timeout = timeout_ms == UINT32_MAX ? INFINITE : (DWORD)timeout_ms;
    BOOL ok = SleepConditionVariableCS(&cond->cv, &mutex->cs, timeout);
    if (ok) return 0;
    return GetLastError() == ERROR_TIMEOUT ? 1 : -1;
}

#else

#  include <errno.h>
#  include <pthread.h>
#  include <time.h>

struct sdrn_thread {
    pthread_t thread;
};
struct sdrn_mutex {
    pthread_mutex_t mutex;
};
struct sdrn_cond {
    pthread_cond_t cond;
};

typedef struct sdrn_thread_start_ctx {
    sdrn_thread_fn fn;
    void *user;
} sdrn_thread_start_ctx;

static void *sdrn_thread_entry(void *opaque) {
    sdrn_thread_start_ctx *ctx = (sdrn_thread_start_ctx *)opaque;
    sdrn_thread_fn fn = ctx->fn;
    void *user = ctx->user;
    free(ctx);
    (void)fn(user);
    return NULL;
}

sdrn_status sdrn_thread_create(sdrn_thread **out_thread, sdrn_thread_fn fn, void *user) {
    if (!out_thread || !fn) return SDRN_ERR_INVALID_ARGUMENT;
    *out_thread = NULL;
    sdrn_thread *thread = (sdrn_thread *)calloc(1, sizeof(*thread));
    sdrn_thread_start_ctx *ctx = (sdrn_thread_start_ctx *)calloc(1, sizeof(*ctx));
    if (!thread || !ctx) {
        free(thread);
        free(ctx);
        return SDRN_ERR_NO_MEMORY;
    }
    ctx->fn = fn;
    ctx->user = user;
    int rc = pthread_create(&thread->thread, NULL, sdrn_thread_entry, ctx);
    if (rc != 0) {
        free(ctx);
        free(thread);
        return SDRN_ERR_INTERNAL;
    }
    *out_thread = thread;
    return SDRN_OK;
}

void sdrn_thread_join_destroy(sdrn_thread *thread) {
    if (!thread) return;
    (void)pthread_join(thread->thread, NULL);
    free(thread);
}

sdrn_status sdrn_mutex_create(sdrn_mutex **out_mutex) {
    if (!out_mutex) return SDRN_ERR_INVALID_ARGUMENT;
    *out_mutex = NULL;
    sdrn_mutex *mutex = (sdrn_mutex *)calloc(1, sizeof(*mutex));
    if (!mutex) return SDRN_ERR_NO_MEMORY;
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) {
        free(mutex);
        return SDRN_ERR_INTERNAL;
    }
    *out_mutex = mutex;
    return SDRN_OK;
}

void sdrn_mutex_destroy(sdrn_mutex *mutex) {
    if (!mutex) return;
    (void)pthread_mutex_destroy(&mutex->mutex);
    free(mutex);
}

void sdrn_mutex_lock(sdrn_mutex *mutex) {
    if (mutex) (void)pthread_mutex_lock(&mutex->mutex);
}

void sdrn_mutex_unlock(sdrn_mutex *mutex) {
    if (mutex) (void)pthread_mutex_unlock(&mutex->mutex);
}

sdrn_status sdrn_cond_create(sdrn_cond **out_cond) {
    if (!out_cond) return SDRN_ERR_INVALID_ARGUMENT;
    *out_cond = NULL;
    sdrn_cond *cond = (sdrn_cond *)calloc(1, sizeof(*cond));
    if (!cond) return SDRN_ERR_NO_MEMORY;
    if (pthread_cond_init(&cond->cond, NULL) != 0) {
        free(cond);
        return SDRN_ERR_INTERNAL;
    }
    *out_cond = cond;
    return SDRN_OK;
}

void sdrn_cond_destroy(sdrn_cond *cond) {
    if (!cond) return;
    (void)pthread_cond_destroy(&cond->cond);
    free(cond);
}

void sdrn_cond_signal(sdrn_cond *cond) {
    if (cond) (void)pthread_cond_signal(&cond->cond);
}

void sdrn_cond_broadcast(sdrn_cond *cond) {
    if (cond) (void)pthread_cond_broadcast(&cond->cond);
}

static void add_ms(struct timespec *ts, uint32_t timeout_ms) {
    ts->tv_sec += (time_t)(timeout_ms / 1000u);
    ts->tv_nsec += (long)((timeout_ms % 1000u) * 1000000u);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

int sdrn_cond_wait_ms(sdrn_cond *cond, sdrn_mutex *mutex, uint32_t timeout_ms) {
    if (!cond || !mutex) return -1;
    if (timeout_ms == UINT32_MAX) {
        return pthread_cond_wait(&cond->cond, &mutex->mutex) == 0 ? 0 : -1;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    add_ms(&deadline, timeout_ms);
    int rc = pthread_cond_timedwait(&cond->cond, &mutex->mutex, &deadline);
    if (rc == 0) return 0;
    if (rc == ETIMEDOUT) return 1;
    return -1;
}

#endif