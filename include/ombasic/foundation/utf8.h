#ifndef OMBASIC_FOUNDATION_UTF8_H
#define OMBASIC_FOUNDATION_UTF8_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/status.h"

Om_Status om_utf8_decode_next(
    Om_Const_Byte_Slice input,
    size_t *offset,
    uint32_t *codepoint
);
Om_Status om_utf8_validate(Om_Const_Byte_Slice input);

#endif
