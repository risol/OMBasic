#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t om_runtime_number_parse_int_status(const char *text, int64_t radix);
int64_t om_runtime_number_parse_int_offset(const char *text, int64_t radix);
int64_t om_runtime_number_parse_int_value(const char *text, int64_t radix);
int64_t om_runtime_number_parse_float_status(const char *text);
int64_t om_runtime_number_parse_float_offset(const char *text);
double om_runtime_number_parse_float_value(const char *text);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024 * 1024) return 0;
    char *text = malloc(size + 1);
    if (text == NULL) return 0;
    if (size != 0) memcpy(text, data, size);
    text[size] = '\0';
    for (int64_t radix = 0; radix <= 40; ++radix) {
        const int64_t status = om_runtime_number_parse_int_status(text, radix);
        (void)om_runtime_number_parse_int_offset(text, radix);
        if (status == 0) {
            (void)om_runtime_number_parse_int_value(text, radix);
        }
    }
    if (om_runtime_number_parse_float_status(text) == 0) {
        (void)om_runtime_number_parse_float_value(text);
    }
    (void)om_runtime_number_parse_float_offset(text);
    free(text);
    return 0;
}
