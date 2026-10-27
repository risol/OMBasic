#include "ombasic/foundation/utf8.h"

#include <stdbool.h>

static bool is_continuation(uint8_t byte)
{
    return (byte & UINT8_C(0xc0)) == UINT8_C(0x80);
}

Om_Status om_utf8_decode_next(
    Om_Const_Byte_Slice input,
    size_t *offset,
    uint32_t *codepoint
)
{
    size_t index = 0;
    size_t width = 0;
    uint32_t value = 0;
    uint8_t first = 0;

    if (offset == NULL || codepoint == NULL || (input.data == NULL && input.length != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (*offset > input.length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (*offset == input.length) {
        return OM_STATUS_END_OF_INPUT;
    }
    index = *offset;
    first = input.data[index];
    if (first <= UINT8_C(0x7f)) {
        width = 1;
        value = first;
    } else if (first >= UINT8_C(0xc2) && first <= UINT8_C(0xdf)) {
        width = 2;
        value = (uint32_t)(first & UINT8_C(0x1f));
    } else if (first >= UINT8_C(0xe0) && first <= UINT8_C(0xef)) {
        width = 3;
        value = (uint32_t)(first & UINT8_C(0x0f));
    } else if (first >= UINT8_C(0xf0) && first <= UINT8_C(0xf4)) {
        width = 4;
        value = (uint32_t)(first & UINT8_C(0x07));
    } else {
        return OM_STATUS_INVALID_UTF8;
    }
    if (width > input.length - index) {
        return OM_STATUS_INVALID_UTF8;
    }
    for (size_t part = 1; part < width; ++part) {
        const uint8_t next = input.data[index + part];
        if (!is_continuation(next)) {
            return OM_STATUS_INVALID_UTF8;
        }
        if (part == 1) {
            if ((first == UINT8_C(0xe0) && next < UINT8_C(0xa0))
                || (first == UINT8_C(0xed) && next > UINT8_C(0x9f))
                || (first == UINT8_C(0xf0) && next < UINT8_C(0x90))
                || (first == UINT8_C(0xf4) && next > UINT8_C(0x8f))) {
                return OM_STATUS_INVALID_UTF8;
            }
        }
        value = (value << 6) | (uint32_t)(next & UINT8_C(0x3f));
    }
    if (value > UINT32_C(0x10ffff)
        || (value >= UINT32_C(0xd800) && value <= UINT32_C(0xdfff))) {
        return OM_STATUS_INVALID_UTF8;
    }
    *offset = index + width;
    *codepoint = value;
    return OM_STATUS_OK;
}

Om_Status om_utf8_validate(Om_Const_Byte_Slice input)
{
    size_t offset = 0;
    uint32_t codepoint = 0;
    if (input.data == NULL && input.length != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    while (offset < input.length) {
        const Om_Status status = om_utf8_decode_next(input, &offset, &codepoint);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}
