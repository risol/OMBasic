#include <stdint.h>
#include <stdlib.h>

int64_t om_runtime_compression_status(const char *hex, int64_t format,
                                      int64_t decompress);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char alphabet[] = "0123456789abcdef";
    if (size > 1024 * 1024) return 0;
    char *hex = malloc(size * 2 + 1);
    if (hex == NULL) return 0;
    for (size_t index = 0; index < size; ++index) {
        hex[index * 2] = alphabet[data[index] >> 4];
        hex[index * 2 + 1] = alphabet[data[index] & 0x0f];
    }
    hex[size * 2] = '\0';
    (void)om_runtime_compression_status(hex, 0, 1);
    (void)om_runtime_compression_status(hex, 1, 1);
    free(hex);
    return 0;
}
