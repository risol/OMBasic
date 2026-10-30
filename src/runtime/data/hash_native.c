#include "ombasic/foundation/memory.h"

#include <stdint.h>
#include <string.h>

enum { HASH_OK = 0, HASH_INVALID_HEX = 1, HASH_RESOURCE_EXHAUSTED = 2 };
enum { HASH_MAX_INPUT_BYTES = 67108864 };

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

static uint8_t *decode_hex(const char *hex, size_t length, size_t *byte_length,
                           int64_t *status)
{
    *byte_length = 0;
    *status = HASH_OK;
    if (hex == NULL || (length & 1u) != 0) {
        *status = HASH_INVALID_HEX;
        return NULL;
    }
    if (length / 2 > HASH_MAX_INPUT_BYTES) {
        *status = HASH_RESOURCE_EXHAUSTED;
        return NULL;
    }
    const size_t count = length / 2;
    uint8_t *bytes = count == 0 ? NULL : om_alloc(count);
    if (count != 0 && bytes == NULL) {
        *status = HASH_RESOURCE_EXHAUSTED;
        return NULL;
    }
    for (size_t index = 0; index < count; ++index) {
        const int high = hex_digit((unsigned char)hex[index * 2]);
        const int low = hex_digit((unsigned char)hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            *status = HASH_INVALID_HEX;
            om_free(bytes);
            return NULL;
        }
        bytes[index] = (uint8_t)(high * 16 + low);
    }
    *byte_length = count;
    return bytes;
}

static uint32_t rotate_right(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    uint32_t words[64];
    for (size_t index = 0; index < 16; ++index) {
        const size_t offset = index * 4;
        words[index] = ((uint32_t)block[offset] << 24)
            | ((uint32_t)block[offset + 1] << 16)
            | ((uint32_t)block[offset + 2] << 8)
            | (uint32_t)block[offset + 3];
    }
    for (size_t index = 16; index < 64; ++index) {
        const uint32_t x = words[index - 15];
        const uint32_t y = words[index - 2];
        const uint32_t sigma0 = rotate_right(x, 7) ^ rotate_right(x, 18)
            ^ (x >> 3);
        const uint32_t sigma1 = rotate_right(y, 17) ^ rotate_right(y, 19)
            ^ (y >> 10);
        words[index] = words[index - 16] + sigma0 + words[index - 7]
            + sigma1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (size_t index = 0; index < 64; ++index) {
        const uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11)
            ^ rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + sum1 + choose + SHA256_K[index]
            + words[index];
        const uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13)
            ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_bytes(const uint8_t *bytes, size_t length,
                         uint8_t digest[32])
{
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    size_t offset = 0;
    while (length - offset >= 64) {
        sha256_block(state, bytes + offset);
        offset += 64;
    }
    uint8_t tail[128] = {0};
    const size_t remainder = length - offset;
    if (remainder != 0) memcpy(tail, bytes + offset, remainder);
    tail[remainder] = 0x80;
    const size_t tail_length = remainder < 56 ? 64 : 128;
    const uint64_t bit_length = (uint64_t)length * 8u;
    for (size_t index = 0; index < 8; ++index) {
        tail[tail_length - 1 - index] = (uint8_t)(bit_length >> (index * 8));
    }
    sha256_block(state, tail);
    if (tail_length == 128) sha256_block(state, tail + 64);
    for (size_t index = 0; index < 8; ++index) {
        digest[index * 4] = (uint8_t)(state[index] >> 24);
        digest[index * 4 + 1] = (uint8_t)(state[index] >> 16);
        digest[index * 4 + 2] = (uint8_t)(state[index] >> 8);
        digest[index * 4 + 3] = (uint8_t)state[index];
    }
}

int64_t om_runtime_hash_sha256_status(const char *hex)
{
    if (hex == NULL) return HASH_INVALID_HEX;
    const size_t length = strlen(hex);
    if ((length & 1u) != 0) return HASH_INVALID_HEX;
    if (length / 2 > HASH_MAX_INPUT_BYTES) return HASH_RESOURCE_EXHAUSTED;
    for (size_t index = 0; index < length; ++index) {
        if (hex_digit((unsigned char)hex[index]) < 0) return HASH_INVALID_HEX;
    }
    return HASH_OK;
}

char *om_runtime_hash_sha256_hex(const char *hex)
{
    int64_t status = HASH_OK;
    size_t byte_length = 0;
    uint8_t *bytes = decode_hex(hex, hex == NULL ? 0 : strlen(hex),
                                &byte_length, &status);
    if (status != HASH_OK) return NULL;
    uint8_t digest[32];
    sha256_bytes(bytes, byte_length, digest);
    om_free(bytes);
    static const char alphabet[] = "0123456789abcdef";
    char *output = om_alloc(65);
    if (output == NULL) return NULL;
    for (size_t index = 0; index < sizeof(digest); ++index) {
        output[index * 2] = alphabet[digest[index] >> 4];
        output[index * 2 + 1] = alphabet[digest[index] & 15u];
    }
    output[64] = '\0';
    return output;
}

int64_t om_runtime_hash_crc32_status(const char *hex)
{
    return om_runtime_hash_sha256_status(hex);
}

int64_t om_runtime_hash_crc32(const char *hex)
{
    int64_t status = HASH_OK;
    size_t byte_length = 0;
    uint8_t *bytes = decode_hex(hex, hex == NULL ? 0 : strlen(hex),
                                &byte_length, &status);
    if (status != HASH_OK) return 0;
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < byte_length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    om_free(bytes);
    return (int64_t)(crc ^ 0xffffffffu);
}

void om_runtime_hash_release(void *value)
{
    om_free(value);
}
