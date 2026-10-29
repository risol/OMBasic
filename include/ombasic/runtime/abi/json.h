#ifndef OMBASIC_RUNTIME_ABI_JSON_H
#define OMBASIC_RUNTIME_ABI_JSON_H

#include <stdbool.h>
#include <stdint.h>

typedef enum Om_Runtime_Json_Kind {
    OM_RUNTIME_JSON_KIND_INVALID = -1,
    OM_RUNTIME_JSON_KIND_NULL = 0,
    OM_RUNTIME_JSON_KIND_BOOLEAN,
    OM_RUNTIME_JSON_KIND_NUMBER,
    OM_RUNTIME_JSON_KIND_STRING,
    OM_RUNTIME_JSON_KIND_ARRAY,
    OM_RUNTIME_JSON_KIND_OBJECT
} Om_Runtime_Json_Kind;

typedef enum Om_Runtime_Json_Error {
    OM_RUNTIME_JSON_ERROR_NONE = 0,
    OM_RUNTIME_JSON_ERROR_SYNTAX = 1,
    OM_RUNTIME_JSON_ERROR_UTF8 = 2,
    OM_RUNTIME_JSON_ERROR_DEPTH = 3,
    OM_RUNTIME_JSON_ERROR_SIZE = 4,
    OM_RUNTIME_JSON_ERROR_DUPLICATE_KEY = 5,
    OM_RUNTIME_JSON_ERROR_TYPE_OR_LOOKUP = 6
} Om_Runtime_Json_Error;

/* JSON Native helpers accept NUL-terminated UTF-8 values and return owned
   strings through om_runtime_json_release. */
bool om_runtime_json_valid(const char *source);
int64_t om_runtime_json_kind(const char *source);
int64_t om_runtime_json_error_code(const char *source);
int64_t om_runtime_json_error_offset(const char *source);
char *om_runtime_json_error_message(const char *source);
char *om_runtime_json_compact(const char *source);
char *om_runtime_json_quote(const char *source);
int64_t om_runtime_json_array_length(const char *source);
bool om_runtime_json_array_index_valid(const char *source, int64_t index);
char *om_runtime_json_array_get(const char *source, int64_t index);
bool om_runtime_json_object_key_valid(const char *source, const char *key);
char *om_runtime_json_object_get(const char *source, const char *key);
void om_runtime_json_release(void *value);

#endif
