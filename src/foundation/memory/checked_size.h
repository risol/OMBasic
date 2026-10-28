#ifndef OMBASIC_FOUNDATION_CHECKED_SIZE_H
#define OMBASIC_FOUNDATION_CHECKED_SIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(OM_HAVE_STDCKDINT_H)
#include <stdckdint.h>

static inline bool om_size_add_overflows(size_t *result,
                                         size_t left,
                                         size_t right)
{
    return ckd_add(result, left, right);
}

static inline bool om_size_mul_overflows(size_t *result,
                                         size_t left,
                                         size_t right)
{
    return ckd_mul(result, left, right);
}
#elif defined(OM_HAVE_CHECKED_SIZE_BUILTINS)
static inline bool om_size_add_overflows(size_t *result,
                                         size_t left,
                                         size_t right)
{
    return __builtin_add_overflow(left, right, result);
}

static inline bool om_size_mul_overflows(size_t *result,
                                         size_t left,
                                         size_t right)
{
    return __builtin_mul_overflow(left, right, result);
}
#elif defined(_MSC_VER)
static inline bool om_size_add_overflows(size_t *result,
                                         size_t left,
                                         size_t right)
{
    if (left > SIZE_MAX - right) {
        return true;
    }
    *result = left + right;
    return false;
}

static inline bool om_size_mul_overflows(size_t *result,
                                         size_t left,
                                         size_t right)
{
    if (right != 0 && left > SIZE_MAX / right) {
        return true;
    }
    *result = left * right;
    return false;
}
#else
#error "A checked size-arithmetic implementation is required"
#endif

#endif
