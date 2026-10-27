#ifndef OM_TOOLING_BIND_API_H
#define OM_TOOLING_BIND_API_H

#include <stdint.h>
#include <stddef.h>

int32_t om_sample_add(int32_t left, int32_t right);
size_t om_sample_length(const char *value);
const char *om_sample_name(void);

#endif
