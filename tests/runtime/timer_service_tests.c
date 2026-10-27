#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "timer_service.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
#endif

typedef struct Om_Timer_Probe {
    atomic_size_t fired_count;
    atomic_size_t cancelled_count;
    atomic_size_t destroy_count;
    atomic_int first_status;
    atomic_int second_status;
    atomic_int next_order;
    atomic_int first_order;
    atomic_int second_order;
} Om_Timer_Probe;

typedef struct Om_Timer_Context {
    Om_Timer_Probe *probe;
    bool first;
} Om_Timer_Context;

static void yield_thread(void)
{
#if defined(_WIN32)
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static bool wait_for_count(atomic_size_t *count, size_t expected)
{
#if defined(_WIN32)
    const ULONGLONG start = GetTickCount64();
#else
    struct timespec start = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return false;
    }
#endif
    while (atomic_load_explicit(count, memory_order_acquire) < expected) {
#if defined(_WIN32)
        if (GetTickCount64() - start > 3000) {
            return false;
        }
#else
        struct timespec now = {0};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0
            || now.tv_sec - start.tv_sec > 3) {
            return false;
        }
#endif
        yield_thread();
    }
    return true;
}

static void timer_complete(void *opaque, Om_Status status)
{
    Om_Timer_Context *context = opaque;
    Om_Timer_Probe *probe = context->probe;
    atomic_store_explicit(context->first ? &probe->first_status
                                         : &probe->second_status,
                          (int)status, memory_order_release);
    if (status == OM_STATUS_OK) {
        const int order = atomic_fetch_add_explicit(
            &probe->next_order, 1, memory_order_acq_rel) + 1;
        atomic_store_explicit(context->first ? &probe->first_order
                                             : &probe->second_order,
                              order, memory_order_release);
        (void)atomic_fetch_add_explicit(&probe->fired_count, 1,
                                        memory_order_release);
    } else if (status == OM_STATUS_CANCELLED) {
        (void)atomic_fetch_add_explicit(&probe->cancelled_count, 1,
                                        memory_order_release);
    }
}

static void timer_destroy(void *opaque)
{
    Om_Timer_Context *context = opaque;
    (void)atomic_fetch_add_explicit(&context->probe->destroy_count, 1,
                                    memory_order_release);
    free(context);
}

static bool schedule_task(Om_Timer_Service *service, Om_Timer_Probe *probe,
                          uint64_t delay, bool first, uint64_t *timer_id)
{
    Om_Timer_Context context = {.probe = probe, .first = first};
    Om_Timer_Context *owned = malloc(sizeof(*owned));
    if (owned == NULL) {
        return false;
    }
    *owned = context;
    const Om_Status status = om_timer_service_schedule(
        service, (Om_Timer_Task){
            .delay_milliseconds = delay,
            .complete = timer_complete,
            .destroy = timer_destroy,
            .context = owned
        }, timer_id);
    if (status != OM_STATUS_OK) {
        free(owned);
        return false;
    }
    return true;
}

static bool test_timer_order_and_cancel(void)
{
    Om_Timer_Service *service = NULL;
    Om_Timer_Probe probe = {0};
    uint64_t first_id = 0;
    uint64_t second_id = 0;
    uint64_t cancel_id = 0;
    if (om_timer_service_create(&service, 4) != OM_STATUS_OK) {
        return false;
    }
    const bool scheduled = schedule_task(service, &probe, 80, true,
                                          &first_id)
        && schedule_task(service, &probe, 10, false, &second_id)
        && schedule_task(service, &probe, 5000, true, &cancel_id);
    const bool cancelled = scheduled
        && om_timer_service_cancel(service, cancel_id) == OM_STATUS_OK
        && om_timer_service_cancel(service, cancel_id) == OM_STATUS_NOT_FOUND;
    const bool completed = cancelled
        && wait_for_count(&probe.fired_count, 2)
        && wait_for_count(&probe.destroy_count, 3);
    const int first_order = atomic_load_explicit(&probe.first_order,
                                                 memory_order_acquire);
    const int second_order = atomic_load_explicit(&probe.second_order,
                                                  memory_order_acquire);
    const int first_status = atomic_load_explicit(&probe.first_status,
                                                  memory_order_acquire);
    const int second_status = atomic_load_explicit(&probe.second_status,
                                                   memory_order_acquire);
    const size_t cancelled_count = atomic_load_explicit(
        &probe.cancelled_count, memory_order_acquire);
    om_timer_service_destroy(service);
    return completed && first_order == 2 && second_order == 1
        && first_status == OM_STATUS_OK && second_status == OM_STATUS_OK
        && cancelled_count == 1;
}

static bool test_shutdown_cancels_pending(void)
{
    Om_Timer_Service *service = NULL;
    Om_Timer_Probe probe = {0};
    uint64_t first_id = 0;
    uint64_t second_id = 0;
    if (om_timer_service_create(&service, 2) != OM_STATUS_OK) {
        return false;
    }
    const bool scheduled = schedule_task(service, &probe, 60000, true,
                                          &first_id)
        && schedule_task(service, &probe, 60000, false, &second_id);
    if (scheduled) {
        om_timer_service_shutdown(service);
    }
    const size_t cancelled_count = atomic_load_explicit(
        &probe.cancelled_count, memory_order_acquire);
    const size_t destroy_count = atomic_load_explicit(
        &probe.destroy_count, memory_order_acquire);
    const bool rejected = om_timer_service_schedule(
        service, (Om_Timer_Task){.delay_milliseconds = 0,
                                 .complete = timer_complete},
        &first_id) == OM_STATUS_CANCELLED;
    om_timer_service_destroy(service);
    return scheduled && rejected && cancelled_count == 2 && destroy_count == 2;
}

int main(void)
{
    if (!test_timer_order_and_cancel()) {
        fputs("timer service ordering/cancel test failed\n", stderr);
        return 1;
    }
    if (!test_shutdown_cancels_pending()) {
        fputs("timer service shutdown test failed\n", stderr);
        return 1;
    }
    puts("timer service tests passed");
    return 0;
}
