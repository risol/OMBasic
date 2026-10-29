#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/runtime/abi/json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const size_t maximum_input_size = (size_t)1024 * (size_t)1024;
    if (size > maximum_input_size || size == SIZE_MAX) return 0;

    char *source = malloc(size + 1);
    if (source == NULL) return 0;
    for (size_t index = 0; index < size; ++index) {
        source[index] = data[index] == 0 ? '\x01' : (char)data[index];
    }
    source[size] = '\0';

    if (om_runtime_json_valid(source)) {
        const int64_t kind = om_runtime_json_kind(source);
        if (kind < OM_RUNTIME_JSON_KIND_NULL
            || kind > OM_RUNTIME_JSON_KIND_OBJECT) {
            abort();
        }
        char *compact = om_runtime_json_compact(source);
        if (compact != NULL) {
            if (!om_runtime_json_valid(compact)
                || om_runtime_json_kind(compact) != kind) {
                abort();
            }
            om_runtime_json_release(compact);
        }
        if (kind == OM_RUNTIME_JSON_KIND_ARRAY) {
            const int64_t length = om_runtime_json_array_length(source);
            if (length < 0) abort();
            if (length > 0) {
                char *item = om_runtime_json_array_get(source, length - 1);
                if (item != NULL) {
                    if (!om_runtime_json_valid(item)) abort();
                    om_runtime_json_release(item);
                }
            }
        } else if (kind == OM_RUNTIME_JSON_KIND_OBJECT) {
            char *item = om_runtime_json_object_get(source, "");
            if (item != NULL) {
                if (!om_runtime_json_valid(item)) abort();
                om_runtime_json_release(item);
            }
        }
    } else {
        const int64_t code = om_runtime_json_error_code(source);
        const int64_t offset = om_runtime_json_error_offset(source);
        if (code <= OM_RUNTIME_JSON_ERROR_NONE
            || code > OM_RUNTIME_JSON_ERROR_TYPE_OR_LOOKUP
            || offset < 0 || (uint64_t)offset > (uint64_t)size) {
            abort();
        }
        char *message = om_runtime_json_error_message(source);
        om_runtime_json_release(message);
    }

    free(source);
    return 0;
}
