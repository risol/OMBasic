#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

char *om_runtime_regex_find(const char *pattern, const char *input);
void om_runtime_regex_release(void *value);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 2048) return 0;
    size_t separator = size == 0 ? 0 : (size_t)data[0] % size;
    const size_t pattern_length = separator;
    const size_t input_length = size - 1 - separator;
    char *pattern = malloc(pattern_length + 1);
    char *input = malloc(input_length + 1);
    if (pattern == NULL || input == NULL) {
        free(pattern);
        free(input);
        return 0;
    }
    if (pattern_length != 0) {
        for (size_t index = 0; index < pattern_length; ++index) {
            pattern[index] = (char)data[index + 1];
        }
    }
    if (input_length != 0) {
        for (size_t index = 0; index < input_length; ++index) {
            input[index] = (char)data[separator + index + 1];
        }
    }
    pattern[pattern_length] = '\0';
    input[input_length] = '\0';
    char *result = om_runtime_regex_find(pattern, input);
    om_runtime_regex_release(result);
    free(pattern);
    free(input);
    return 0;
}
