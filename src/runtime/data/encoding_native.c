#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/foundation/utf8.h"

#include <stdint.h>
#include <string.h>

enum {
    UTF8_OK = 0,
    UTF8_INVALID = 1,
    UTF8_RESOURCE_EXHAUSTED = 2,
    UTF8_INVALID_HEX = 3,
};

enum { UTF8_MAX_BYTES = 67108864 };

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

static uint8_t *decode_hex(const char *hex, size_t hex_length,
                           size_t *byte_length, int64_t *error_offset,
                           int64_t *status)
{
    *byte_length = 0;
    *error_offset = 0;
    *status = UTF8_OK;
    if ((hex_length & 1u) != 0) {
        *status = UTF8_INVALID_HEX;
        *error_offset = (int64_t)hex_length;
        return NULL;
    }
    const size_t length = hex_length / 2;
    if (length > UTF8_MAX_BYTES) {
        *status = UTF8_RESOURCE_EXHAUSTED;
        return NULL;
    }
    uint8_t *bytes = length == 0 ? NULL : om_alloc(length);
    if (length != 0 && bytes == NULL) {
        *status = UTF8_RESOURCE_EXHAUSTED;
        return NULL;
    }
    for (size_t index = 0; index < length; ++index) {
        const int high = hex_digit((unsigned char)hex[index * 2]);
        const int low = hex_digit((unsigned char)hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            *status = UTF8_INVALID_HEX;
            *error_offset = (int64_t)(index * 2 + (high < 0 ? 0 : 1));
            om_free(bytes);
            return NULL;
        }
        bytes[index] = (uint8_t)(high * 16 + low);
    }
    *byte_length = length;
    return bytes;
}

static int64_t validate_utf8_bytes(const uint8_t *bytes, size_t byte_length,
                                   int64_t *error_offset)
{
    size_t offset = 0;
    while (offset < byte_length) {
        const size_t scalar_start = offset;
        uint32_t scalar = 0;
        if (om_utf8_decode_next((Om_Const_Byte_Slice){bytes, byte_length},
                                &offset, &scalar) != OM_STATUS_OK) {
            *error_offset = (int64_t)scalar_start;
            return UTF8_INVALID;
        }
    }
    return UTF8_OK;
}

static int64_t validate_hex_utf8(const char *hex, size_t hex_length,
                                 int64_t *error_offset)
{
    size_t byte_length = 0;
    int64_t status = UTF8_OK;
    uint8_t *bytes = decode_hex(hex, hex_length, &byte_length, error_offset,
                               &status);
    if (status != UTF8_OK) return status;
    status = validate_utf8_bytes(bytes, byte_length, error_offset);
    om_free(bytes);
    return status;
}

int64_t om_runtime_encoding_utf8_encode_status(const Om_String_View *value)
{
    if (value == NULL || (value->data == NULL && value->length != 0)) {
        return UTF8_INVALID;
    }
    if (value->length > UTF8_MAX_BYTES) return UTF8_RESOURCE_EXHAUSTED;
    return om_utf8_validate((Om_Const_Byte_Slice){
        (const uint8_t *)value->data, value->length}) == OM_STATUS_OK
        ? UTF8_OK : UTF8_INVALID;
}

char *om_runtime_encoding_utf8_encode_hex(const Om_String_View *value)
{
    static const char alphabet[] = "0123456789abcdef";
    if (om_runtime_encoding_utf8_encode_status(value) != UTF8_OK
        || value->length > (SIZE_MAX - 1) / 2) {
        return NULL;
    }
    const size_t output_length = value->length * 2;
    char *output = om_alloc(output_length + 1);
    if (output == NULL) return NULL;
    const uint8_t *bytes = (const uint8_t *)value->data;
    for (size_t index = 0; index < value->length; ++index) {
        output[index * 2] = alphabet[bytes[index] >> 4];
        output[index * 2 + 1] = alphabet[bytes[index] & 15u];
    }
    output[output_length] = '\0';
    return output;
}

int64_t om_runtime_encoding_utf8_decode_status(const char *hex)
{
    if (hex == NULL) return UTF8_INVALID_HEX;
    int64_t error_offset = 0;
    return validate_hex_utf8(hex, strlen(hex), &error_offset);
}

int64_t om_runtime_encoding_utf8_decode_offset(const char *hex)
{
    if (hex == NULL) return 0;
    int64_t error_offset = 0;
    (void)validate_hex_utf8(hex, strlen(hex), &error_offset);
    return error_offset;
}

Om_String_View *om_runtime_encoding_utf8_decode_slice(const char *hex)
{
    if (hex == NULL) return NULL;
    const size_t hex_length = strlen(hex);
    size_t byte_length = 0;
    int64_t error_offset = 0;
    int64_t status = UTF8_OK;
    uint8_t *bytes = decode_hex(hex, hex_length, &byte_length,
                                &error_offset, &status);
    if (status != UTF8_OK
        || validate_utf8_bytes(bytes, byte_length, &error_offset)
            != UTF8_OK) {
        om_free(bytes);
        return NULL;
    }
    Om_String_View *result = om_alloc(sizeof(*result));
    if (result == NULL) {
        om_free(bytes);
        return NULL;
    }
    result->data = (const char *)bytes;
    result->length = byte_length;
    return result;
}

void om_runtime_encoding_utf8_slice_release(void *value)
{
    Om_String_View *slice = value;
    if (slice == NULL) return;
    om_free((void *)slice->data);
    om_free(slice);
}

void om_runtime_encoding_utf8_string_release(void *value)
{
    om_free(value);
}
