#ifndef OMBASIC_FOUNDATION_VECTOR_H
#define OMBASIC_FOUNDATION_VECTOR_H

#include <stddef.h>

#include "ombasic/foundation/status.h"

typedef struct Om_Vector {
    void *data;
    size_t length;
    size_t capacity;
    size_t item_size;
} Om_Vector;

/* Vector owns its storage. Items are copied byte-for-byte and must not alias data. */
Om_Status om_vector_init(Om_Vector *vector, size_t item_size);
Om_Status om_vector_reserve(Om_Vector *vector, size_t capacity);
Om_Status om_vector_push(Om_Vector *vector, const void *item);
void *om_vector_at(Om_Vector *vector, size_t index);
const void *om_vector_at_const(const Om_Vector *vector, size_t index);
void om_vector_clear(Om_Vector *vector);
void om_vector_destroy(Om_Vector *vector);

#endif
