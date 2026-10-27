#include "ombasic/foundation/vector.h"

#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

Om_Status om_vector_init(Om_Vector *vector, size_t item_size)
{
    if (vector == NULL || item_size == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *vector = (Om_Vector){0};
    vector->item_size = item_size;
    return OM_STATUS_OK;
}

Om_Status om_vector_reserve(Om_Vector *vector, size_t capacity)
{
    size_t bytes = 0;
    void *new_data = NULL;
    if (vector == NULL || vector->item_size == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (capacity <= vector->capacity) {
        return OM_STATUS_OK;
    }
    if (!om_checked_size_mul(capacity, vector->item_size, &bytes)) {
        return OM_STATUS_OVERFLOW;
    }
    (void)bytes;
    new_data = om_realloc_array(vector->data, capacity, vector->item_size);
    if (new_data == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    vector->data = new_data;
    vector->capacity = capacity;
    return OM_STATUS_OK;
}

Om_Status om_vector_push(Om_Vector *vector, const void *item)
{
    size_t new_length = 0;
    size_t new_capacity = 0;
    Om_Status status = OM_STATUS_OK;
    if (vector == NULL || item == NULL || vector->item_size == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (!om_checked_size_add(vector->length, 1, &new_length)) {
        return OM_STATUS_OVERFLOW;
    }
    if (new_length > vector->capacity) {
        if (vector->capacity == 0) {
            new_capacity = 8;
        } else if (!om_checked_size_add(vector->capacity, vector->capacity, &new_capacity)) {
            return OM_STATUS_OVERFLOW;
        }
        if (new_capacity < new_length) {
            new_capacity = new_length;
        }
        status = om_vector_reserve(vector, new_capacity);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    memcpy((unsigned char *)vector->data + vector->length * vector->item_size,
           item,
           vector->item_size);
    vector->length = new_length;
    return OM_STATUS_OK;
}

void *om_vector_at(Om_Vector *vector, size_t index)
{
    if (vector == NULL || index >= vector->length) {
        return NULL;
    }
    return (unsigned char *)vector->data + index * vector->item_size;
}

const void *om_vector_at_const(const Om_Vector *vector, size_t index)
{
    if (vector == NULL || index >= vector->length) {
        return NULL;
    }
    return (const unsigned char *)vector->data + index * vector->item_size;
}

void om_vector_clear(Om_Vector *vector)
{
    if (vector != NULL) {
        vector->length = 0;
    }
}

void om_vector_destroy(Om_Vector *vector)
{
    if (vector == NULL) {
        return;
    }
    om_free(vector->data);
    *vector = (Om_Vector){0};
}
