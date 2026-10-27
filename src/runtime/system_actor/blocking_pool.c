#include "blocking_pool.h"

#include <stdbool.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#define OM_BLOCKING_POOL_MAX_WORKERS ((size_t)64)

typedef struct Om_Blocking_Task_Node {
    Om_Blocking_Task task;
    struct Om_Blocking_Task_Node *next;
} Om_Blocking_Task_Node;

typedef struct Om_Blocking_Thread {
#if defined(_WIN32)
    HANDLE native;
#else
    pthread_t native;
#endif
} Om_Blocking_Thread;

struct Om_Blocking_Pool {
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
    Om_Blocking_Thread *threads;
    size_t thread_count;
    size_t queue_capacity;
    size_t queued_count;
    Om_Blocking_Task_Node *head;
    Om_Blocking_Task_Node *tail;
    bool stopping;
    bool drain;
    bool shutdown_started;
    bool shutdown_complete;
};

static bool pool_mutex_init(Om_Blocking_Pool *pool)
{
#if defined(_WIN32)
    if (!InitializeCriticalSectionEx(&pool->mutex, 0, 0)) {
        return false;
    }
    InitializeConditionVariable(&pool->condition);
#else
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&pool->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&pool->mutex);
        return false;
    }
#endif
    return true;
}

static void pool_mutex_lock(Om_Blocking_Pool *pool)
{
#if defined(_WIN32)
    EnterCriticalSection(&pool->mutex);
#else
    (void)pthread_mutex_lock(&pool->mutex);
#endif
}

static void pool_mutex_unlock(Om_Blocking_Pool *pool)
{
#if defined(_WIN32)
    LeaveCriticalSection(&pool->mutex);
#else
    (void)pthread_mutex_unlock(&pool->mutex);
#endif
}

static void pool_wait(Om_Blocking_Pool *pool)
{
#if defined(_WIN32)
    (void)SleepConditionVariableCS(&pool->condition, &pool->mutex, INFINITE);
#else
    (void)pthread_cond_wait(&pool->condition, &pool->mutex);
#endif
}

static void pool_broadcast(Om_Blocking_Pool *pool)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&pool->condition);
#else
    (void)pthread_cond_broadcast(&pool->condition);
#endif
}

static void pool_mutex_destroy(Om_Blocking_Pool *pool)
{
#if defined(_WIN32)
    DeleteCriticalSection(&pool->mutex);
#else
    (void)pthread_cond_destroy(&pool->condition);
    (void)pthread_mutex_destroy(&pool->mutex);
#endif
}

static void destroy_task_node(Om_Blocking_Task_Node *node,
                              Om_Status completion_status)
{
    if (node->task.complete != NULL) {
        node->task.complete(node->task.context, completion_status);
    }
    if (node->task.destroy != NULL) {
        node->task.destroy(node->task.context);
    }
    om_free(node);
}

static void run_worker(Om_Blocking_Pool *pool)
{
    for (;;) {
        pool_mutex_lock(pool);
        while (pool->head == NULL && !pool->stopping) {
            pool_wait(pool);
        }
        if (pool->head == NULL && pool->stopping) {
            pool_mutex_unlock(pool);
            return;
        }
        if (pool->stopping && !pool->drain) {
            pool_mutex_unlock(pool);
            return;
        }
        Om_Blocking_Task_Node *node = pool->head;
        pool->head = node->next;
        if (pool->head == NULL) {
            pool->tail = NULL;
        }
        pool->queued_count -= 1;
        pool_mutex_unlock(pool);

        const Om_Status status = node->task.run(node->task.context);
        destroy_task_node(node, status);
    }
}

#if defined(_WIN32)
static DWORD WINAPI blocking_worker_entry(LPVOID argument)
{
    run_worker((Om_Blocking_Pool *)argument);
    return 0;
}

static bool create_worker(Om_Blocking_Thread *thread,
                          Om_Blocking_Pool *pool)
{
    thread->native = CreateThread(NULL, 0, blocking_worker_entry, pool,
                                  0, NULL);
    return thread->native != NULL;
}

static void join_worker(Om_Blocking_Thread *thread)
{
    (void)WaitForSingleObject(thread->native, INFINITE);
    (void)CloseHandle(thread->native);
}
#else
static void *blocking_worker_entry(void *argument)
{
    run_worker((Om_Blocking_Pool *)argument);
    return NULL;
}

static bool create_worker(Om_Blocking_Thread *thread,
                          Om_Blocking_Pool *pool)
{
    return pthread_create(&thread->native, NULL, blocking_worker_entry, pool)
        == 0;
}

static void join_worker(Om_Blocking_Thread *thread)
{
    (void)pthread_join(thread->native, NULL);
}
#endif

Om_Status om_blocking_pool_create(Om_Blocking_Pool **pool,
                                  size_t worker_count,
                                  size_t queue_capacity)
{
    if (pool == NULL || *pool != NULL || worker_count == 0
        || queue_capacity == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (worker_count > OM_BLOCKING_POOL_MAX_WORKERS) {
        worker_count = OM_BLOCKING_POOL_MAX_WORKERS;
    }
    Om_Blocking_Pool *created = om_alloc(sizeof(*created));
    if (created == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(created, 0, sizeof(*created));
    created->threads = om_alloc_array(worker_count, sizeof(*created->threads));
    if (created->threads == NULL) {
        om_free(created);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (!pool_mutex_init(created)) {
        om_free(created->threads);
        om_free(created);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    created->queue_capacity = queue_capacity;
    for (size_t index = 0; index < worker_count; ++index) {
        if (!create_worker(&created->threads[index], created)) {
            break;
        }
        created->thread_count += 1;
    }
    if (created->thread_count == 0) {
        pool_mutex_destroy(created);
        om_free(created->threads);
        om_free(created);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *pool = created;
    return OM_STATUS_OK;
}

Om_Status om_blocking_pool_submit(Om_Blocking_Pool *pool,
                                  Om_Blocking_Task task)
{
    if (pool == NULL || task.run == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Blocking_Task_Node *node = om_alloc(sizeof(*node));
    if (node == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    node->task = task;
    node->next = NULL;

    pool_mutex_lock(pool);
    if (pool->stopping) {
        pool_mutex_unlock(pool);
        om_free(node);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (pool->queued_count >= pool->queue_capacity) {
        pool_mutex_unlock(pool);
        om_free(node);
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (pool->tail == NULL) {
        pool->head = node;
    } else {
        pool->tail->next = node;
    }
    pool->tail = node;
    pool->queued_count += 1;
    pool_broadcast(pool);
    pool_mutex_unlock(pool);
    return OM_STATUS_OK;
}

void om_blocking_pool_shutdown(Om_Blocking_Pool *pool,
                               Om_Blocking_Pool_Shutdown_Mode mode)
{
    if (pool == NULL) {
        return;
    }
    pool_mutex_lock(pool);
    if (pool->shutdown_started) {
        while (!pool->shutdown_complete) {
            pool_wait(pool);
        }
        pool_mutex_unlock(pool);
        return;
    }
    pool->shutdown_started = true;
    pool->stopping = true;
    pool->drain = mode == OM_BLOCKING_POOL_DRAIN;
    Om_Blocking_Task_Node *cancelled = NULL;
    if (!pool->drain) {
        cancelled = pool->head;
        pool->head = NULL;
        pool->tail = NULL;
        pool->queued_count = 0;
    }
    pool_broadcast(pool);
    pool_mutex_unlock(pool);

    while (cancelled != NULL) {
        Om_Blocking_Task_Node *next = cancelled->next;
        destroy_task_node(cancelled, OM_STATUS_CANCELLED);
        cancelled = next;
    }
    for (size_t index = 0; index < pool->thread_count; ++index) {
        join_worker(&pool->threads[index]);
    }
    pool_mutex_lock(pool);
    pool->thread_count = 0;
    pool->shutdown_complete = true;
    pool_broadcast(pool);
    pool_mutex_unlock(pool);
}

void om_blocking_pool_destroy(Om_Blocking_Pool *pool)
{
    if (pool == NULL) {
        return;
    }
    om_blocking_pool_shutdown(pool, OM_BLOCKING_POOL_CANCEL_PENDING);
    Om_Blocking_Task_Node *node = pool->head;
    while (node != NULL) {
        Om_Blocking_Task_Node *next = node->next;
        destroy_task_node(node, OM_STATUS_CANCELLED);
        node = next;
    }
    pool_mutex_destroy(pool);
    om_free(pool->threads);
    om_free(pool);
}
