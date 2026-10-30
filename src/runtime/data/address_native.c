#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include "ombasic/foundation/memory.h"

#include <stdint.h>
#include <string.h>

enum { ADDRESS_OK = 0, ADDRESS_INVALID = 1, ADDRESS_MAX_TEXT = 128 };

static int address_inet_pton(int family, const char *text, void *bytes)
{
#if defined(_WIN32)
    return InetPtonA(family, text, bytes);
#else
    return inet_pton(family, text, bytes);
#endif
}

static const char *address_inet_ntop(int family, const void *bytes,
                                     char *text, size_t length)
{
#if defined(_WIN32)
    return InetNtopA(family, (void *)bytes, text, (DWORD)length);
#else
    return inet_ntop(family, bytes, text, (socklen_t)length);
#endif
}

static int address_parse(const char *text, uint8_t *family,
                         uint8_t bytes[16])
{
    if (text == NULL || family == NULL || bytes == NULL
        || strlen(text) > ADDRESS_MAX_TEXT) return ADDRESS_INVALID;
    uint8_t parsed[16] = {0};
    if (address_inet_pton(AF_INET, text, parsed) == 1) {
        *family = 4;
        (void)memcpy(bytes, parsed, 4);
        return ADDRESS_OK;
    }
    if (address_inet_pton(AF_INET6, text, parsed) == 1) {
        *family = 6;
        (void)memcpy(bytes, parsed, 16);
        return ADDRESS_OK;
    }
    return ADDRESS_INVALID;
}

int64_t om_runtime_address_ip_status(const char *text)
{
    uint8_t family = 0;
    uint8_t bytes[16] = {0};
    return address_parse(text, &family, bytes);
}

int64_t om_runtime_address_ip_family(const char *text)
{
    uint8_t family = 0;
    uint8_t bytes[16] = {0};
    return address_parse(text, &family, bytes) == ADDRESS_OK ? family : 0;
}

int64_t om_runtime_address_ip_byte(const char *text, int64_t index)
{
    uint8_t family = 0;
    uint8_t bytes[16] = {0};
    if (index < 0 || address_parse(text, &family, bytes) != ADDRESS_OK) {
        return -1;
    }
    const size_t length = family == 4 ? 4 : 16;
    return (uint64_t)index < length ? bytes[index] : -1;
}

char *om_runtime_address_ip_bytes_hex(const char *text)
{
    uint8_t family = 0;
    uint8_t bytes[16] = {0};
    if (address_parse(text, &family, bytes) != ADDRESS_OK) return NULL;
    static const char digits[] = "0123456789abcdef";
    const size_t length = family == 4 ? 4 : 16;
    char *hex = om_alloc(length * 2 + 1);
    if (hex == NULL) return NULL;
    for (size_t index = 0; index < length; ++index) {
        hex[index * 2] = digits[bytes[index] >> 4];
        hex[index * 2 + 1] = digits[bytes[index] & 15];
    }
    hex[length * 2] = '\0';
    return hex;
}

char *om_runtime_address_ip_format(int64_t family, const char *hex)
{
    if (hex == NULL || (family != 4 && family != 6)) return NULL;
    const size_t count = family == 4 ? 4 : 16;
    if (strlen(hex) != count * 2) return NULL;
    uint8_t bytes[16] = {0};
    for (size_t index = 0; index < count; ++index) {
        const char high_char = hex[index * 2];
        const char low_char = hex[index * 2 + 1];
        int high = high_char >= '0' && high_char <= '9' ? high_char - '0'
            : high_char >= 'a' && high_char <= 'f' ? high_char - 'a' + 10
            : high_char >= 'A' && high_char <= 'F' ? high_char - 'A' + 10 : -1;
        int low = low_char >= '0' && low_char <= '9' ? low_char - '0'
            : low_char >= 'a' && low_char <= 'f' ? low_char - 'a' + 10
            : low_char >= 'A' && low_char <= 'F' ? low_char - 'A' + 10 : -1;
        if (high < 0 || low < 0) return NULL;
        bytes[index] = (uint8_t)(high * 16 + low);
    }
    char text[INET6_ADDRSTRLEN] = {0};
    const int native_family = family == 4 ? AF_INET : AF_INET6;
    if (address_inet_ntop(native_family, bytes, text, sizeof(text)) == NULL) {
        return NULL;
    }
    const size_t length = strlen(text);
    char *copy = om_alloc(length + 1);
    if (copy != NULL) (void)memcpy(copy, text, length + 1);
    return copy;
}

void om_runtime_address_release(void *value)
{
    om_free(value);
}
