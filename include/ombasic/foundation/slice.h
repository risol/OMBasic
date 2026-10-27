#ifndef OMBASIC_FOUNDATION_SLICE_H
#define OMBASIC_FOUNDATION_SLICE_H

#include <stddef.h>
#include <stdint.h>

/* Slices are borrowed ranges; their owner controls the storage lifetime. */
typedef struct Om_Const_Byte_Slice {
    const uint8_t *data;
    size_t length;
} Om_Const_Byte_Slice;

typedef struct Om_Byte_Slice {
    uint8_t *data;
    size_t length;
} Om_Byte_Slice;

#endif
