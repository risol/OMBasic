#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "timer_service.h"

#include <stdbool.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

typedef struct Om_Timer_Node {
    uint64_t id;
    uint64_t deadline_milliseconds;
    Om_Timer_Task task;
    struct Om_Timer_Node *next;
} Om_Timer_Node;

typedef struct Om_Timer_Thread {
#if defined(_WIN32)
    HANDLE native;
#else
    pthread_t native;
#endif
} Om_Timer_Thread;

struct Om_Timer_Service {
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
    Om_Timer_Thread thread;
    Om_Timer_Node *head;
    size_t queued_count;
    size_t queue_capacity;
    uint64_t next_id;
    bool stopping;
    bool shutdown_started;
    bool shutdown_complete;
};

static uint64_t monotonic_milliseconds(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000)
        + (uint64_t)now.tv_nsec / UINT64_C(1000000);
#endif
}

static bool timer_mutex_init(Om_Timer_Service *service)
{
#if defined(_WIN32)
    if (!InitializeCriticalSectionEx(&service->mutex, 0, 0)) {
        return false;
    }
    InitializeConditionVariable(&service->condition);
#else
    if (pthread_mutex_init(&service->mutex, NULL) != 0) {
        return false;
    }
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes) != 0) {
        (void)pthread_mutex_destroy(&service->mutex);
        return false;
    }
#if defined(CLOCK_MONOTONIC) && !defined(__APPLE__)
    const int clock_status = pthread_condattr_setclock(
        &attributes, CLOCK_MONOTONIC);
    if (clock_status != 0) {
        (void)pthread_condattr_destroy(&attributes);
        (void)pthread_mutex_destroy(&service->mutex);
        return false;
    }
#endif
    const int condition_status = pthread_cond_init(
        &service->condition, &attributes);
    (void)pthread_condattr_destroy(&attributes);
    if (condition_status != 0) {
        (void)pthread_mutex_destroy(&service->mutex);
        return false;
    }
#endif
    return true;
}

static void timer_mutex_lock(Om_Timer_Service *service)
{
#if defined(_WIN32)
    EnterCriticalSection(&service->mutex);
#else
    (void)pthread_mutex_lock(&service->mutex);
#endif
}

static void timer_mutex_unlock(Om_Timer_Service *service)
{
#if defined(_WIN32)
    LeaveCriticalSection(&service->mutex);
#else
    (void)pthread_mutex_unlock(&service->mutex);
#endif
}

static void timer_condition_wait(Om_Timer_Service *service)
{
#if defined(_WIN32)
    (void)SleepConditionVariableCS(&service->condition, &service->mutex,
                                   INFINITE);
#else
    (void)pthread_cond_wait(&service->condition, &service->mutex);
#endif
}

static void timer_condition_wait_until(Om_Timer_Service *service,
                                       uint64_t deadline_milliseconds)
{
#if defined(_WIN32)
    const uint64_t now = monotonic_milliseconds();
    const uint64_t remaining = deadline_milliseconds > now
        ? deadline_milliseconds - now : 0;
    const DWORD timeout = remaining >= (uint64_t)INFINITE - 1
        ? INFINITE - 1 : (DWORD)remaining;
    (void)SleepConditionVariableCS(&service->condition, &service->mutex,
                                   timeout);
#else
    const struct timespec deadline = {
        .tv_sec = (time_t)(deadline_milliseconds / UINT64_C(1000)),
        .tv_nsec = (long)((deadline_milliseconds % UINT64_C(1000))
                          * UINT64_C(1000000))
    };
    (void)pthread_cond_timedwait(&service->condition, &service->mutex,
                                 &deadline);
#endif
}

static void timer_condition_broadcast(Om_Timer_Service *service)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&service->condition);
#else
    (void)pthread_cond_broadcast(&service->condition);
#endif
}

static void timer_mutex_destroy(Om_Timer_Service *service)
{
#if defined(_WIN32)
    DeleteCriticalSection(&service->mutex);
#else
    (void)pthread_cond_destroy(&service->condition);
    (void)pthread_mutex_destroy(&service->mutex);
#endif
}

static void finish_task(Om_Timer_Node *node, Om_Status status)
{
    if (node->task.complete != NULL) {
        node->task.complete(node->task.context, status);
    }
    if (node->task.destroy != NULL) {
        node->task.destroy(node->task.context);
    }
    om_free(node);
}

static void run_timer_service(Om_Timer_Service *service)
{
    for (;;) {
        timer_mutex_lock(service);
        while (service->head == NULL && !service->stopping) {
            timer_condition_wait(service);
        }
        if (service->stopping) {
            timer_mutex_unlock(service);
            return;
        }
        const uint64_t now = monotonic_milliseconds();
        if (service->head->deadline_milliseconds > now) {
            timer_condition_wait_until(
                service, service->head->deadline_milliseconds);
            timer_mutex_unlock(service);
            continue;
        }
        Om_Timer_Node *node = service->head;
        service->head = node->next;
        service->queued_count -= 1;
        timer_mutex_unlock(service);
        finish_task(node, OM_STATUS_OK);
    }
}

#if defined(_WIN32)
static DWORD WINAPI timer_thread_entry(LPVOID argument)
{
    run_timer_service((Om_Timer_Service *)argument);
    return 0;
}

static bool create_timer_thread(Om_Timer_Service *service)
{
    service->thread.native = CreateThread(NULL, 0, timer_thread_entry,
                                          service, 0, NULL);
    return service->thread.native != NULL;
}

static void join_timer_thread(Om_Timer_Service *service)
{
    (void)WaitForSingleObject(service->thread.native, INFINITE);
    (void)CloseHandle(service->thread.native);
}
#else
static void *timer_thread_entry(void *argument)
{
    run_timer_service((Om_Timer_Service *)argument);
    return NULL;
}

static bool create_timer_thread(Om_Timer_Service *service)
{
    return pthread_create(&service->thread.native, NULL, timer_thread_entry,
                          service) == 0;
}

static void join_timer_thread(Om_Timer_Service *service)
{
    (void)pthread_join(service->thread.native, NULL);
}
#endif

Om_Status om_timer_service_create(Om_Timer_Service **service,
                                  size_t queue_capacity)
{
    if (service == NULL || *service != NULL || queue_capacity == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Timer_Service *created = om_alloc(sizeof(*created));
    if (created == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(created, 0, sizeof(*created));
    if (!timer_mutex_init(created)) {
        om_free(created);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    created->queue_capacity = queue_capacity;
    created->next_id = 1;
    if (!create_timer_thread(created)) {
        timer_mutex_destroy(created);
        om_free(created);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *service = created;
    return OM_STATUS_OK;
}

Om_Status om_timer_service_schedule(Om_Timer_Service *service,
                                    Om_Timer_Task task,
                                    uint64_t *timer_id)
{
    if (service == NULL || task.complete == NULL || timer_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Timer_Node *node = om_alloc(sizeof(*node));
    if (node == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const uint64_t now = monotonic_milliseconds();
    if (task.delay_milliseconds > UINT64_MAX - now) {
        om_free(node);
        return OM_STATUS_OVERFLOW;
    }
    node->deadline_milliseconds = now + task.delay_milliseconds;
    node->task = task;
    node->next = NULL;

    timer_mutex_lock(service);
    if (service->stopping) {
        timer_mutex_unlock(service);
        om_free(node);
        return OM_STATUS_CANCELLED;
    }
    if (service->queued_count >= service->queue_capacity) {
        timer_mutex_unlock(service);
        om_free(node);
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (service->next_id == 0 || service->next_id == UINT64_MAX) {
        timer_mutex_unlock(service);
        om_free(node);
        return OM_STATUS_OVERFLOW;
    }
    node->id = service->next_id++;
    Om_Timer_Node **cursor = &service->head;
    while (*cursor != NULL
           && (*cursor)->deadline_milliseconds <= node->deadline_milliseconds) {
        cursor = &(*cursor)->next;
    }
    node->next = *cursor;
    *cursor = node;
    service->queued_count += 1;
    *timer_id = node->id;
    timer_condition_broadcast(service);
    timer_mutex_unlock(service);
    return OM_STATUS_OK;
}

Om_Status om_timer_service_cancel(Om_Timer_Service *service,
                                  uint64_t timer_id)
{
    if (service == NULL || timer_id == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    timer_mutex_lock(service);
    Om_Timer_Node **cursor = &service->head;
    while (*cursor != NULL && (*cursor)->id != timer_id) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == NULL) {
        timer_mutex_unlock(service);
        return OM_STATUS_NOT_FOUND;
    }
    Om_Timer_Node *node = *cursor;
    *cursor = node->next;
    service->queued_count -= 1;
    timer_condition_broadcast(service);
    timer_mutex_unlock(service);
    finish_task(node, OM_STATUS_CANCELLED);
    return OM_STATUS_OK;
}

void om_timer_service_shutdown(Om_Timer_Service *service)
{
    if (service == NULL) {
        return;
    }
    timer_mutex_lock(service);
    if (service->shutdown_started) {
        while (!service->shutdown_complete) {
            timer_condition_wait(service);
        }
        timer_mutex_unlock(service);
        return;
    }
    service->shutdown_started = true;
    service->stopping = true;
    Om_Timer_Node *cancelled = service->head;
    service->head = NULL;
    service->queued_count = 0;
    timer_condition_broadcast(service);
    timer_mutex_unlock(service);

    while (cancelled != NULL) {
        Om_Timer_Node *next = cancelled->next;
        finish_task(cancelled, OM_STATUS_CANCELLED);
        cancelled = next;
    }
    join_timer_thread(service);

    timer_mutex_lock(service);
    service->shutdown_complete = true;
    timer_condition_broadcast(service);
    timer_mutex_unlock(service);
}

void om_timer_service_destroy(Om_Timer_Service *service)
{
    if (service == NULL) {
        return;
    }
    om_timer_service_shutdown(service);
    timer_mutex_destroy(service);
    om_free(service);
}
