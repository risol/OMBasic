#ifndef OMBASIC_FOUNDATION_MEMORY_H
#define OMBASIC_FOUNDATION_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

bool om_checked_size_add(size_t left, size_t right, size_t *out);
bool om_checked_size_mul(size_t left, size_t right, size_t *out);

/* Zero-sized allocation requests return NULL. */
void *om_alloc(size_t size);
void *om_alloc_array(size_t count, size_t item_size);

/*
 * A zero count frees ptr and returns NULL. Overflow or allocation failure
 * returns NULL and preserves ptr, matching realloc's failure ownership rule.
 */
void *om_realloc_array(void *ptr, size_t count, size_t item_size);
void om_free(void *ptr);

#endif
