#ifndef OMBASIC_RUNTIME_ABI_TEXT_H
#define OMBASIC_RUNTIME_ABI_TEXT_H

#include <stdbool.h>
#include <stdint.h>

/* Native Text helpers accept NUL-terminated, valid UTF-8 borrowed strings. */
int64_t om_runtime_text_length(const char *value);
bool om_runtime_text_starts_with(const char *value, const char *prefix);
bool om_runtime_text_ends_with(const char *value, const char *suffix);
int64_t om_runtime_text_find(const char *value, const char *needle,
                             int64_t start_index);
int64_t om_runtime_text_find_last(const char *value, const char *needle);
char *om_runtime_text_slice(const char *value, int64_t start_index,
                            int64_t scalar_count);
void om_runtime_text_release(void *value);

#endif
