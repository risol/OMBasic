#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>
#endif

int om_ffi_add(int left, int right)
{
    return left + right;
}

double om_ffi_mixed(int left, double scale, int right)
{
    return (double)left + scale * (double)right;
}

size_t om_ffi_utf8_length(const char *value)
{
    return value == NULL ? 0 : strlen(value);
}

const char *om_ffi_echo_string(const char *value)
{
    return value;
}

int8_t om_ffi_echo_i8(int8_t value)
{
    return value;
}

typedef struct Om_Ffi_Point {
    int32_t x;
    double y;
} Om_Ffi_Point;

Om_Ffi_Point om_ffi_echo_point(Om_Ffi_Point value)
{
    value.x += 2;
    value.y *= 3.0;
    return value;
}

char *native_create_name(void)
{
    static const char value[] = "native-name";
    char *copy = malloc(sizeof(value));
    if (copy != NULL) {
        (void)memcpy(copy, value, sizeof(value));
    }
    return copy;
}

void native_release_name(void *value)
{
    free(value);
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

static atomic_bool fast_actor_seen = false;

void om_ffi_reset_fast_actor_seen(void)
{
    atomic_store_explicit(&fast_actor_seen, false, memory_order_release);
}

void om_ffi_mark_fast_actor_seen(void)
{
    atomic_store_explicit(&fast_actor_seen, true, memory_order_release);
}

int32_t om_ffi_wait_for_fast_actor(int32_t timeout_ms)
{
    if (timeout_ms < 0) {
        return 0;
    }
    for (int32_t elapsed = 0; elapsed <= timeout_ms; ++elapsed) {
        if (atomic_load_explicit(&fast_actor_seen, memory_order_acquire)) {
            return 1;
        }
        if (elapsed == timeout_ms) {
            break;
        }
#if defined(_WIN32)
        (void)Sleep(1);
#else
        const struct timespec one_millisecond = {0, 1000000};
        (void)nanosleep(&one_millisecond, NULL);
#endif
    }
    return 0;
}

typedef void (*Om_Native_Test_Callback)(int value, void *context);

typedef struct Om_Native_Test_Callback_Task {
    Om_Native_Test_Callback callback;
    void *context;
} Om_Native_Test_Callback_Task;

#if defined(_WIN32)
static DWORD WINAPI run_callback_thread(LPVOID opaque)
{
    Om_Native_Test_Callback_Task *task = opaque;
    task->callback(42, task->context);
    return 0;
}
#else
static void *run_callback_thread(void *opaque)
{
    Om_Native_Test_Callback_Task *task = opaque;
    task->callback(42, task->context);
    return NULL;
}
#endif

int om_ffi_invoke_callback_on_thread(Om_Native_Test_Callback callback,
                                     void *context)
{
    if (callback == NULL) {
        return -1;
    }
    Om_Native_Test_Callback_Task task = {callback, context};
#if defined(_WIN32)
    HANDLE thread = CreateThread(NULL, 0, run_callback_thread, &task, 0, NULL);
    if (thread == NULL) {
        return -1;
    }
    const DWORD wait_status = WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
    return wait_status == WAIT_OBJECT_0 ? 0 : -1;
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, run_callback_thread, &task) != 0) {
        return -1;
    }
    if (pthread_join(thread, NULL) != 0) {
        return -1;
    }
    return 0;
#endif
}

typedef void (*Om_Native_Test_Single_Callback)(int value);

typedef struct Om_Native_Test_Single_Callback_Task {
    Om_Native_Test_Single_Callback callback;
} Om_Native_Test_Single_Callback_Task;

#if defined(_WIN32)
static DWORD WINAPI run_single_callback_thread(LPVOID opaque)
{
    Om_Native_Test_Single_Callback_Task *task = opaque;
    task->callback(42);
    return 0;
}
#else
static void *run_single_callback_thread(void *opaque)
{
    Om_Native_Test_Single_Callback_Task *task = opaque;
    task->callback(42);
    return NULL;
}
#endif

void om_ffi_invoke_single_callback_on_thread(
    Om_Native_Test_Single_Callback callback)
{
    if (callback == NULL) {
        return;
    }
    Om_Native_Test_Single_Callback_Task task = {callback};
#if defined(_WIN32)
    HANDLE thread = CreateThread(
        NULL, 0, run_single_callback_thread, &task, 0, NULL);
    if (thread == NULL) {
        return;
    }
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, run_single_callback_thread, &task) != 0) {
        return;
    }
    (void)pthread_join(thread, NULL);
#endif
}
