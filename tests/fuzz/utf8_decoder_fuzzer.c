#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Om_String_View {
    const char *data;
    size_t length;
} Om_String_View;

int64_t om_runtime_encoding_utf8_decode_status(const char *hex);
int64_t om_runtime_encoding_utf8_decode_offset(const char *hex);
Om_String_View *om_runtime_encoding_utf8_decode_slice(const char *hex);
void om_runtime_encoding_utf8_slice_release(void *value);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 2 * 1024 * 1024) return 0;
    char *hex = malloc(size + 1);
    if (hex == NULL) return 0;
    if (size != 0) memcpy(hex, data, size);
    hex[size] = '\0';
    (void)om_runtime_encoding_utf8_decode_status(hex);
    (void)om_runtime_encoding_utf8_decode_offset(hex);
    Om_String_View *decoded = om_runtime_encoding_utf8_decode_slice(hex);
    om_runtime_encoding_utf8_slice_release(decoded);
    free(hex);
    return 0;
}
