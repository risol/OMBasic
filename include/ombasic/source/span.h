#ifndef OMBASIC_SOURCE_SPAN_H
#define OMBASIC_SOURCE_SPAN_H

#include <stdbool.h>
#include <stdint.h>

#include "ombasic/foundation/ids.h"

/* Byte offsets are half-open and always refer to one source file. */
typedef struct Om_Source_Span {
    Om_Source_Id source;
    uint32_t start;
    uint32_t end;
} Om_Source_Span;

bool om_source_span_is_valid(Om_Source_Span span, uint32_t source_length);

#endif
