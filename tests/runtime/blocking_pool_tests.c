#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "blocking_pool.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#endif

typedef struct Om_Blocking_Pool_Probe {
    atomic_size_t entered;
    atomic_size_t active;
    atomic_size_t maximum_active;
    atomic_size_t run_count;
    atomic_size_t completion_count;
    atomic_size_t cancellation_count;
    atomic_size_t destroy_count;
    atomic_size_t error_count;
    atomic_bool release;
} Om_Blocking_Pool_Probe;

static void yield_thread(void)
{
#if defined(_WIN32)
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static bool wait_for_entered(Om_Blocking_Pool_Probe *probe, size_t expected)
{
#if defined(_WIN32)
    const ULONGLONG start_ms = GetTickCount64();
#else
    struct timespec start = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return false;
    }
#endif
    for (;;) {
        if (atomic_load_explicit(&probe->entered, memory_order_acquire)
            >= expected) {
            return true;
        }
#if defined(_WIN32)
        if (GetTickCount64() - start_ms >= UINT64_C(10000)) {
            return false;
        }
#else
        struct timespec current = {0};
        if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
            return false;
        }
        const int64_t elapsed_ms =
            ((int64_t)current.tv_sec - (int64_t)start.tv_sec) * INT64_C(1000)
            + ((int64_t)current.tv_nsec - (int64_t)start.tv_nsec)
                / INT64_C(1000000);
        if (elapsed_ms >= INT64_C(10000)) {
            return false;
        }
#endif
        yield_thread();
    }
}

static Om_Status probe_task_run(void *context)
{
    Om_Blocking_Pool_Probe *probe = context;
    const size_t active = atomic_fetch_add_explicit(
        &probe->active, 1, memory_order_acq_rel) + 1;
    size_t maximum = atomic_load_explicit(
        &probe->maximum_active, memory_order_relaxed);
    while (maximum < active
           && !atomic_compare_exchange_weak_explicit(
               &probe->maximum_active, &maximum, active,
               memory_order_release, memory_order_relaxed)) {
    }
    (void)atomic_fetch_add_explicit(&probe->run_count, 1,
                                    memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&probe->entered, 1,
                                    memory_order_release);
    while (!atomic_load_explicit(&probe->release, memory_order_acquire)) {
        yield_thread();
    }
    (void)atomic_fetch_sub_explicit(&probe->active, 1, memory_order_acq_rel);
    return OM_STATUS_OK;
}

static void probe_task_complete(void *context, Om_Status status)
{
    Om_Blocking_Pool_Probe *probe = context;
    if (status == OM_STATUS_CANCELLED) {
        (void)atomic_fetch_add_explicit(&probe->cancellation_count, 1,
                                        memory_order_relaxed);
        atomic_store_explicit(&probe->release, true, memory_order_release);
    } else if (status == OM_STATUS_OK) {
        (void)atomic_fetch_add_explicit(&probe->completion_count, 1,
                                        memory_order_relaxed);
    } else {
        (void)atomic_fetch_add_explicit(&probe->error_count, 1,
                                        memory_order_relaxed);
    }
}

static void probe_task_destroy(void *context)
{
    Om_Blocking_Pool_Probe *probe = context;
    (void)atomic_fetch_add_explicit(&probe->destroy_count, 1,
                                    memory_order_relaxed);
}

static Om_Blocking_Task probe_task(Om_Blocking_Pool_Probe *probe)
{
    return (Om_Blocking_Task){
        .run = probe_task_run,
        .complete = probe_task_complete,
        .destroy = probe_task_destroy,
        .context = probe
    };
}

static bool test_pool_runs_concurrently_and_drains(void)
{
    Om_Blocking_Pool *pool = NULL;
    Om_Blocking_Pool_Probe probe = {0};
    bool passed = false;
    if (om_blocking_pool_create(&pool, 2, 2) != OM_STATUS_OK) {
        goto cleanup;
    }
    if (om_blocking_pool_submit(pool, probe_task(&probe)) != OM_STATUS_OK
        || om_blocking_pool_submit(pool, probe_task(&probe)) != OM_STATUS_OK) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        goto cleanup;
    }
    if (!wait_for_entered(&probe, 2)) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        goto cleanup;
    }
    if (atomic_load_explicit(&probe.maximum_active, memory_order_acquire) < 2) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        goto cleanup;
    }
    atomic_store_explicit(&probe.release, true, memory_order_release);
    om_blocking_pool_shutdown(pool, OM_BLOCKING_POOL_DRAIN);
    passed = atomic_load_explicit(&probe.run_count, memory_order_relaxed) == 2
        && atomic_load_explicit(&probe.completion_count,
                                memory_order_relaxed) == 2
        && atomic_load_explicit(&probe.cancellation_count,
                                memory_order_relaxed) == 0
        && atomic_load_explicit(&probe.destroy_count,
                                memory_order_relaxed) == 2
        && atomic_load_explicit(&probe.error_count,
                                memory_order_relaxed) == 0;
cleanup:
    if (pool != NULL) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        om_blocking_pool_destroy(pool);
    }
    return passed;
}

static bool test_pool_cancels_pending_and_bounds_queue(void)
{
    Om_Blocking_Pool *pool = NULL;
    Om_Blocking_Pool_Probe probe = {0};
    bool passed = false;
    if (om_blocking_pool_create(&pool, 1, 2) != OM_STATUS_OK) {
        goto cleanup;
    }
    if (om_blocking_pool_submit(pool, probe_task(&probe)) != OM_STATUS_OK
        || !wait_for_entered(&probe, 1)) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        goto cleanup;
    }
    if (om_blocking_pool_submit(pool, probe_task(&probe)) != OM_STATUS_OK
        || om_blocking_pool_submit(pool, probe_task(&probe)) != OM_STATUS_OK
        || om_blocking_pool_submit(pool, probe_task(&probe))
            != OM_STATUS_OUT_OF_RANGE) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        goto cleanup;
    }
    om_blocking_pool_shutdown(pool, OM_BLOCKING_POOL_CANCEL_PENDING);
    passed = atomic_load_explicit(&probe.run_count, memory_order_relaxed) == 1
        && atomic_load_explicit(&probe.completion_count,
                                memory_order_relaxed) == 1
        && atomic_load_explicit(&probe.cancellation_count,
                                memory_order_relaxed) == 2
        && atomic_load_explicit(&probe.destroy_count,
                                memory_order_relaxed) == 3
        && atomic_load_explicit(&probe.error_count,
                                memory_order_relaxed) == 0;
cleanup:
    if (pool != NULL) {
        atomic_store_explicit(&probe.release, true, memory_order_release);
        om_blocking_pool_destroy(pool);
    }
    return passed;
}

int main(void)
{
    size_t failures = 0;
    if (test_pool_runs_concurrently_and_drains()) {
        (void)puts("PASS blocking_pool_concurrency_and_drain");
    } else {
        (void)fprintf(stderr, "FAIL blocking_pool_concurrency_and_drain\n");
        failures += 1;
    }
    if (test_pool_cancels_pending_and_bounds_queue()) {
        (void)puts("PASS blocking_pool_cancel_pending_and_queue_bound");
    } else {
        (void)fprintf(stderr, "FAIL blocking_pool_cancel_pending_and_queue_bound\n");
        failures += 1;
    }
    (void)printf("2 tests, %zu failures\n", failures);
    return failures == 0 ? 0 : 1;
}
