#include "ombasic/foundation/memory.h"

#include <stdckdint.h>
#include <stdlib.h>

bool om_checked_size_add(size_t left, size_t right, size_t *out)
{
    size_t result = 0;
    if (out == NULL) {
        return false;
    }
    *out = 0;
    if (ckd_add(&result, left, right)) {
        return false;
    }
    *out = result;
    return true;
}

bool om_checked_size_mul(size_t left, size_t right, size_t *out)
{
    size_t result = 0;
    if (out == NULL) {
        return false;
    }
    *out = 0;
    if (ckd_mul(&result, left, right)) {
        return false;
    }
    *out = result;
    return true;
}

void *om_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    return malloc(size);
}

void *om_alloc_array(size_t count, size_t item_size)
{
    size_t size = 0;
    if (count == 0 || item_size == 0 || !om_checked_size_mul(count, item_size, &size)) {
        return NULL;
    }
    return om_alloc(size);
}

void *om_realloc_array(void *ptr, size_t count, size_t item_size)
{
    size_t size = 0;
    if (count == 0) {
        free(ptr);
        return NULL;
    }
    if (item_size == 0 || !om_checked_size_mul(count, item_size, &size)) {
        return NULL;
    }
    return realloc(ptr, size);
}

void om_free(void *ptr)
{
    free(ptr);
}
