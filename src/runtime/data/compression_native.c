#include "ombasic/foundation/memory.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
    COMPRESSION_OK = 0,
    COMPRESSION_INVALID_FORMAT = 1,
    COMPRESSION_CHECKSUM_MISMATCH = 2,
    COMPRESSION_RESOURCE_EXHAUSTED = 3,
    COMPRESSION_INVALID_BUFFER = 4,
    COMPRESSION_UNSUPPORTED = 5,
};

enum {
    COMPRESSION_GZIP = 0,
    COMPRESSION_DEFLATE = 1,
    COMPRESSION_MAX_BYTES = 64 * 1024 * 1024,
    COMPRESSION_INITIAL_OUTPUT = 16384,
};

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

static uint8_t *decode_hex(const char *hex, size_t length, size_t *byte_count,
                           int64_t *status)
{
    *byte_count = 0;
    if (hex == NULL || (length & 1u) != 0) {
        *status = COMPRESSION_INVALID_BUFFER;
        return NULL;
    }
    if (length / 2 > COMPRESSION_MAX_BYTES) {
        *status = COMPRESSION_RESOURCE_EXHAUSTED;
        return NULL;
    }
    const size_t count = length / 2;
    uint8_t *bytes = count == 0 ? NULL : om_alloc(count);
    if (count != 0 && bytes == NULL) {
        *status = COMPRESSION_RESOURCE_EXHAUSTED;
        return NULL;
    }
    for (size_t index = 0; index < count; ++index) {
        const int high = hex_digit((unsigned char)hex[index * 2]);
        const int low = hex_digit((unsigned char)hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            *status = COMPRESSION_INVALID_BUFFER;
            om_free(bytes);
            return NULL;
        }
        bytes[index] = (uint8_t)((high << 4) | low);
    }
    *byte_count = count;
    *status = COMPRESSION_OK;
    return bytes;
}

static char *encode_hex(const uint8_t *bytes, size_t length)
{
    static const char alphabet[] = "0123456789abcdef";
    size_t hex_length = 0;
    if (length > COMPRESSION_MAX_BYTES
        || !om_checked_size_mul(length, 2, &hex_length)
        || hex_length == SIZE_MAX) {
        return NULL;
    }
    char *hex = om_alloc(hex_length + 1);
    if (hex == NULL) return NULL;
    for (size_t index = 0; index < length; ++index) {
        hex[index * 2] = alphabet[bytes[index] >> 4];
        hex[index * 2 + 1] = alphabet[bytes[index] & 0x0fu];
    }
    hex[hex_length] = '\0';
    return hex;
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint32_t read_u32_be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
        | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static int compress_bytes(const uint8_t *input, size_t input_length,
                          bool gzip, uint8_t **result,
                          size_t *result_length)
{
    *result = NULL;
    *result_length = 0;
    if (input_length > COMPRESSION_MAX_BYTES) {
        return COMPRESSION_RESOURCE_EXHAUSTED;
    }
    if (input_length > UINT_MAX) return COMPRESSION_RESOURCE_EXHAUSTED;

    mz_stream stream = {0};
    const int window_bits = gzip ? -MZ_DEFAULT_WINDOW_BITS
                                 : MZ_DEFAULT_WINDOW_BITS;
    if (mz_deflateInit2(&stream, MZ_DEFAULT_LEVEL, MZ_DEFLATED, window_bits,
                        8, MZ_DEFAULT_STRATEGY) != MZ_OK) {
        return COMPRESSION_RESOURCE_EXHAUSTED;
    }
    const size_t prefix_length = gzip ? 10u : 0u;
    const size_t suffix_length = gzip ? 8u : 0u;
    const size_t max_payload = COMPRESSION_MAX_BYTES - prefix_length - suffix_length;
    const mz_ulong bound_value = mz_deflateBound(&stream, (mz_ulong)input_length);
    size_t capacity = 0;
    if (bound_value > max_payload) {
        capacity = COMPRESSION_MAX_BYTES;
    } else {
        capacity = prefix_length + (size_t)bound_value + suffix_length;
    }
    if (capacity == 0) capacity = 1;
    uint8_t *output = om_alloc(capacity);
    if (output == NULL) {
        (void)mz_deflateEnd(&stream);
        return COMPRESSION_RESOURCE_EXHAUSTED;
    }
    if (gzip) {
        static const uint8_t header[10] = {
            0x1fu, 0x8bu, 0x08u, 0, 0, 0, 0, 0, 0, 0x03u
        };
        memcpy(output, header, sizeof(header));
    }
    stream.next_in = input;
    stream.avail_in = (unsigned int)input_length;
    stream.next_out = output + prefix_length;
    stream.avail_out = (unsigned int)(capacity - prefix_length - suffix_length);
    const int deflate_status = mz_deflate(&stream, MZ_FINISH);
    const size_t payload_length = (size_t)stream.total_out;
    const size_t consumed = (size_t)stream.total_in;
    const unsigned int remaining_output = stream.avail_out;
    (void)mz_deflateEnd(&stream);
    if (deflate_status != MZ_STREAM_END || consumed != input_length) {
        om_free(output);
        if (deflate_status == MZ_MEM_ERROR) {
            return COMPRESSION_RESOURCE_EXHAUSTED;
        }
        return remaining_output == 0 ? COMPRESSION_RESOURCE_EXHAUSTED
                                     : COMPRESSION_INVALID_FORMAT;
    }
    size_t total_length = prefix_length + payload_length;
    if (gzip) {
        const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, input,
                                                input_length);
        write_u32_le(output + total_length, crc);
        total_length += 4;
        write_u32_le(output + total_length, (uint32_t)input_length);
        total_length += 4;
    }
    *result = output;
    *result_length = total_length;
    return COMPRESSION_OK;
}

static bool read_zero_terminated_field(const uint8_t *input, size_t length,
                                       size_t *cursor)
{
    while (*cursor < length) {
        if (input[(*cursor)++] == 0) return true;
    }
    return false;
}

static int gzip_payload(const uint8_t *input, size_t length,
                        size_t *payload_offset, size_t *payload_length,
                        uint32_t *expected_crc, uint32_t *expected_size)
{
    if (length < 18 || input[0] != 0x1fu || input[1] != 0x8bu
        || input[2] != 8) {
        return COMPRESSION_INVALID_FORMAT;
    }
    const uint8_t flags = input[3];
    if ((flags & 0xe0u) != 0) return COMPRESSION_INVALID_FORMAT;
    size_t cursor = 10;
    const size_t trailer_offset = length - 8;
    if ((flags & 0x04u) != 0) {
        if (cursor + 2 > trailer_offset) return COMPRESSION_INVALID_FORMAT;
        const size_t extra_length = (size_t)input[cursor]
            | ((size_t)input[cursor + 1] << 8);
        cursor += 2;
        if (extra_length > trailer_offset - cursor) {
            return COMPRESSION_INVALID_FORMAT;
        }
        cursor += extra_length;
    }
    if ((flags & 0x08u) != 0
        && !read_zero_terminated_field(input, trailer_offset, &cursor)) {
        return COMPRESSION_INVALID_FORMAT;
    }
    if ((flags & 0x10u) != 0
        && !read_zero_terminated_field(input, trailer_offset, &cursor)) {
        return COMPRESSION_INVALID_FORMAT;
    }
    if ((flags & 0x02u) != 0) {
        if (cursor + 2 > trailer_offset) return COMPRESSION_INVALID_FORMAT;
        const uint16_t header_crc = (uint16_t)mz_crc32(
            MZ_CRC32_INIT, input, cursor);
        const uint16_t expected_header_crc = (uint16_t)(
            (uint32_t)input[cursor]
            | ((uint32_t)input[cursor + 1] << 8));
        cursor += 2;
        if (header_crc != expected_header_crc) {
            return COMPRESSION_CHECKSUM_MISMATCH;
        }
    }
    if (cursor > trailer_offset) return COMPRESSION_INVALID_FORMAT;
    *payload_offset = cursor;
    *payload_length = trailer_offset - cursor;
    *expected_crc = read_u32_le(input + trailer_offset);
    *expected_size = read_u32_le(input + trailer_offset + 4);
    return COMPRESSION_OK;
}

static int decompress_bytes(const uint8_t *input, size_t input_length,
                            bool gzip, uint8_t **result,
                            size_t *result_length)
{
    *result = NULL;
    *result_length = 0;
    if (input_length == 0) return COMPRESSION_INVALID_FORMAT;
    size_t payload_offset = 0;
    size_t payload_length = input_length;
    uint32_t expected_checksum = 0;
    uint32_t expected_size = 0;
    bool check_adler = false;
    if (gzip) {
        const int status = gzip_payload(input, input_length, &payload_offset,
                                        &payload_length, &expected_checksum,
                                        &expected_size);
        if (status != COMPRESSION_OK) return status;
    } else {
        if (input_length < 6) return COMPRESSION_INVALID_FORMAT;
        const uint8_t cmf = input[0];
        const uint8_t flg = input[1];
        if ((cmf & 0x0fu) != 8 || (cmf >> 4) > 7
            || ((((uint32_t)cmf << 8) | (uint32_t)flg) % 31u) != 0) {
            return COMPRESSION_INVALID_FORMAT;
        }
        if ((flg & 0x20u) != 0) return COMPRESSION_UNSUPPORTED;
        payload_offset = 2;
        payload_length = input_length - 6;
        expected_checksum = read_u32_be(input + input_length - 4);
        check_adler = true;
    }
    if (payload_length == 0 || payload_length > UINT_MAX) {
        return COMPRESSION_INVALID_FORMAT;
    }

    size_t capacity = COMPRESSION_INITIAL_OUTPUT;
    uint8_t *output = om_alloc(capacity);
    if (output == NULL) return COMPRESSION_RESOURCE_EXHAUSTED;
    mz_stream stream = {0};
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        om_free(output);
        return COMPRESSION_RESOURCE_EXHAUSTED;
    }
    stream.next_in = input + payload_offset;
    stream.avail_in = (unsigned int)payload_length;
    int inflate_status = MZ_OK;
    bool output_limit = false;
    for (;;) {
        const size_t produced_now = (size_t)stream.total_out;
        if (produced_now >= COMPRESSION_MAX_BYTES + 1u) {
            output_limit = true;
            break;
        }
        stream.next_out = output + produced_now;
        const size_t available = capacity - produced_now;
        stream.avail_out = available > UINT_MAX
            ? UINT_MAX : (unsigned int)available;
        inflate_status = mz_inflate(&stream, MZ_NO_FLUSH);
        if (inflate_status == MZ_STREAM_END) break;
        if ((size_t)stream.total_out > COMPRESSION_MAX_BYTES) {
            output_limit = true;
            break;
        }
        if (stream.avail_out == 0) {
            if (capacity == COMPRESSION_MAX_BYTES + 1u) {
                output_limit = true;
                break;
            }
            size_t next_capacity = capacity * 2;
            if (next_capacity < capacity
                || next_capacity > COMPRESSION_MAX_BYTES + 1u) {
                next_capacity = COMPRESSION_MAX_BYTES + 1u;
            }
            uint8_t *grown = om_realloc_array(output, next_capacity,
                                               sizeof(*output));
            if (grown == NULL) {
                (void)mz_inflateEnd(&stream);
                om_free(output);
                return COMPRESSION_RESOURCE_EXHAUSTED;
            }
            output = grown;
            capacity = next_capacity;
            continue;
        }
        if (inflate_status != MZ_OK || stream.avail_in == 0) break;
    }
    const size_t produced = (size_t)stream.total_out;
    const size_t consumed = (size_t)stream.total_in;
    (void)mz_inflateEnd(&stream);
    if (output_limit || produced > COMPRESSION_MAX_BYTES) {
        om_free(output);
        return COMPRESSION_RESOURCE_EXHAUSTED;
    }
    if (inflate_status != MZ_STREAM_END || consumed != payload_length) {
        om_free(output);
        if (inflate_status == MZ_MEM_ERROR) {
            return COMPRESSION_RESOURCE_EXHAUSTED;
        }
        return COMPRESSION_INVALID_FORMAT;
    }
    if (gzip) {
        const uint32_t actual_crc = (uint32_t)mz_crc32(
            MZ_CRC32_INIT, output, produced);
        if (actual_crc != expected_checksum
            || (uint32_t)produced != expected_size) {
            om_free(output);
            return COMPRESSION_CHECKSUM_MISMATCH;
        }
    } else if (check_adler) {
        const uint32_t actual_adler = (uint32_t)mz_adler32(
            MZ_ADLER32_INIT, output, produced);
        if (actual_adler != expected_checksum) {
            om_free(output);
            return COMPRESSION_CHECKSUM_MISMATCH;
        }
    }
    *result = output;
    *result_length = produced;
    return COMPRESSION_OK;
}

static int transform_status(const char *hex, int64_t format, bool decompress)
{
    if (format != COMPRESSION_GZIP && format != COMPRESSION_DEFLATE) {
        return COMPRESSION_UNSUPPORTED;
    }
    size_t input_length = 0;
    int64_t status = COMPRESSION_OK;
    uint8_t *input = decode_hex(hex, hex == NULL ? 0 : strlen(hex),
                                &input_length, &status);
    if (status != COMPRESSION_OK) return (int)status;
    uint8_t *output = NULL;
    size_t output_length = 0;
    status = decompress
        ? decompress_bytes(input, input_length, format == COMPRESSION_GZIP,
                           &output, &output_length)
        : compress_bytes(input, input_length, format == COMPRESSION_GZIP,
                         &output, &output_length);
    om_free(input);
    om_free(output);
    return (int)status;
}

static char *transform_hex(const char *hex, int64_t format, bool decompress)
{
    size_t input_length = 0;
    int64_t status = COMPRESSION_OK;
    uint8_t *input = decode_hex(hex, hex == NULL ? 0 : strlen(hex),
                                &input_length, &status);
    if (status != COMPRESSION_OK) return NULL;
    uint8_t *output = NULL;
    size_t output_length = 0;
    status = decompress
        ? decompress_bytes(input, input_length, format == COMPRESSION_GZIP,
                           &output, &output_length)
        : compress_bytes(input, input_length, format == COMPRESSION_GZIP,
                         &output, &output_length);
    om_free(input);
    if (status != COMPRESSION_OK) {
        om_free(output);
        return NULL;
    }
    char *result = encode_hex(output, output_length);
    om_free(output);
    return result;
}

int64_t om_runtime_compression_status(const char *hex, int64_t format,
                                      int64_t decompress)
{
    return (int64_t)transform_status(hex, format, decompress != 0);
}

char *om_runtime_compression_transform(const char *hex, int64_t format,
                                       int64_t decompress)
{
    return transform_hex(hex, format, decompress != 0);
}

void om_runtime_compression_release(void *value)
{
    om_free(value);
}
