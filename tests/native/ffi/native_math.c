#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef void (*Om_Ffi_Callback)(int8_t signed_value,
                               uint8_t unsigned_value,
                               float float_value,
                               bool boolean_value);

#if defined(_WIN32)
static DWORD WINAPI om_ffi_callback_thread(void *context)
#else
static void *om_ffi_callback_thread(void *context)
#endif
{
    Om_Ffi_Callback callback = *(Om_Ffi_Callback *)context;
    callback(-7, 250, 1.25f, true);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

int32_t om_ffi_invoke_callback_on_thread(Om_Ffi_Callback callback)
{
    if (callback == NULL) return -1;
#if defined(_WIN32)
    Sleep(20);
    HANDLE thread = CreateThread(NULL, 0, om_ffi_callback_thread,
                                 &callback, 0, NULL);
    if (thread == NULL) return -1;
    const DWORD wait_result = WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
    return wait_result == WAIT_OBJECT_0 ? 0 : -1;
#else
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000};
    (void)nanosleep(&delay, NULL);
    pthread_t thread;
    if (pthread_create(&thread, NULL, om_ffi_callback_thread,
                       &callback) != 0) {
        return -1;
    }
    return pthread_join(thread, NULL) == 0 ? 0 : -1;
#endif
}

int32_t om_ffi_add(int32_t left, int32_t right)
{
    return left + right;
}

int32_t om_ffi_cstring_length(const char *value)
{
    return (int32_t)strlen(value);
}

int8_t om_ffi_add_i8(int8_t left, int8_t right)
{
    return (int8_t)(left + right);
}

uint8_t om_ffi_add_u8(uint8_t left, uint8_t right)
{
    return (uint8_t)(left + right);
}

typedef struct Om_Ffi_Point {
    double x;
    double y;
} Om_Ffi_Point;

typedef struct Om_Ffi_Mixed {
    int32_t integer;
    float real;
} Om_Ffi_Mixed;

typedef struct Om_Ffi_Floats {
    float x;
    float y;
} Om_Ffi_Floats;

typedef struct Om_Ffi_Large {
    uint64_t first;
    uint64_t second;
    uint64_t third;
} Om_Ffi_Large;

typedef struct Om_Ffi_Handle {
    void *pointer;
    uint32_t length;
} Om_Ffi_Handle;

Om_Ffi_Point om_ffi_make_point(void)
{
    return (Om_Ffi_Point){1.25, 2.75};
}

int32_t om_ffi_take_point(Om_Ffi_Point point)
{
    return point.x == 1.25 && point.y == 2.75 ? 1 : 0;
}

Om_Ffi_Mixed om_ffi_make_mixed(void)
{
    return (Om_Ffi_Mixed){11, 2.5f};
}

int32_t om_ffi_take_mixed(Om_Ffi_Mixed value)
{
    return value.integer == 11 && value.real == 2.5f ? 1 : 0;
}

Om_Ffi_Floats om_ffi_make_floats(void)
{
    return (Om_Ffi_Floats){1.5f, 2.5f};
}

int32_t om_ffi_take_floats(Om_Ffi_Floats value)
{
    return value.x == 1.5f && value.y == 2.5f ? 1 : 0;
}

Om_Ffi_Large om_ffi_make_large(void)
{
    return (Om_Ffi_Large){11, 22, 33};
}

int32_t om_ffi_take_large(Om_Ffi_Large value)
{
    return value.first == 11 && value.second == 22 && value.third == 33
        ? 1 : 0;
}

Om_Ffi_Handle om_ffi_make_handle(void)
{
    return (Om_Ffi_Handle){NULL, 7};
}

int32_t om_ffi_take_handle(Om_Ffi_Handle value)
{
    return value.pointer == NULL && value.length == 7 ? 1 : 0;
}

const char *om_ffi_echo_cstring(const char *value)
{
    return value;
}

char *om_ffi_duplicate_cstring(const char *value)
{
    const size_t length = strlen(value);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        (void)memcpy(copy, value, length + 1);
    }
    return copy;
}

void om_ffi_free_cstring(void *pointer)
{
    free(pointer);
}

typedef struct Om_Ffi_Resource {
    int32_t value;
} Om_Ffi_Resource;

static const Om_Ffi_Resource OM_FFI_STATIC_RESOURCE = {77};

void *om_ffi_static_resource(void)
{
    return (void *)&OM_FFI_STATIC_RESOURCE;
}

void *om_ffi_make_resource(int32_t value)
{
    Om_Ffi_Resource *resource = malloc(sizeof(*resource));
    if (resource != NULL) {
        resource->value = value;
    }
    return resource;
}

void *om_ffi_borrow_resource(void *pointer)
{
    return pointer;
}

int32_t om_ffi_read_resource(const void *pointer)
{
    return pointer == NULL ? -1 : ((const Om_Ffi_Resource *)pointer)->value;
}

int32_t om_ffi_consume_resource(void *pointer)
{
    if (pointer == NULL) {
        return -1;
    }
    const int32_t value = ((Om_Ffi_Resource *)pointer)->value;
    free(pointer);
    return value;
}

void om_ffi_release_resource(void *pointer)
{
    free(pointer);
}
