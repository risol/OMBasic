#ifndef OMBASIC_RUNTIME_ABI_DATA_H
#define OMBASIC_RUNTIME_ABI_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/runtime/abi/type_descriptor.h"

typedef union Om_Runtime_Value {
    bool boolean;
    int64_t integer;
    double floating;
    void *managed;
} Om_Runtime_Value;

Om_Status om_runtime_string_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   Om_Const_Byte_Slice utf8,
                                   void **result);
Om_Status om_runtime_string_create_utf8(Om_Execution_Context *context,
                                        const Om_Runtime_Type_Descriptor *type,
                                        const uint8_t *bytes,
                                        size_t length,
                                        void **result);
Om_Status om_runtime_string_create_utf8_default(Om_Execution_Context *context,
                                                const uint8_t *bytes,
                                                size_t length,
                                                void **result);
Om_Status om_runtime_string_concat(Om_Execution_Context *context,
                                   const void *left,
                                   const void *right,
                                   void **result);
Om_Status om_runtime_string_format_value(Om_Execution_Context *context,
                                         Om_Runtime_Type_Kind kind,
                                         const Om_Runtime_Value *value,
                                         void **result);
Om_Status om_runtime_string_view(const Om_Execution_Context *context,
                                 const void *string,
                                 Om_String_View *result);
/* The UTF-8 view is borrowed unless the bytes contain an embedded NUL. */
Om_Status om_runtime_string_cstring_view(const Om_Execution_Context *context,
                                         const void *string,
                                         const uint8_t **bytes,
                                         size_t *length);
Om_Status om_runtime_print(Om_Execution_Context *context,
                           const void *string);

Om_Status om_runtime_record_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   void **result);
Om_Status om_runtime_record_set_value(Om_Execution_Context *context,
                                      void *record,
                                      size_t field_index,
                                      const void *value);
Om_Status om_runtime_record_get(const Om_Execution_Context *context,
                                const void *record,
                                size_t field_index,
                                void *result);

Om_Status om_runtime_list_create(Om_Execution_Context *context,
                                 const Om_Runtime_Type_Descriptor *type,
                                 void **result);
Om_Status om_runtime_list_append_value(Om_Execution_Context *context,
                                      void *list,
                                      const Om_Runtime_Value *value);
Om_Status om_runtime_list_get(const Om_Execution_Context *context,
                              const void *list,
                              size_t index,
                              Om_Runtime_Value *result);
Om_Status om_runtime_list_length(const Om_Execution_Context *context,
                                 const void *list,
                                 size_t *result);

Om_Status om_runtime_map_create(Om_Execution_Context *context,
                                const Om_Runtime_Type_Descriptor *type,
                                void **result);
Om_Status om_runtime_map_set_values(Om_Execution_Context *context,
                                    void *map,
                                    const Om_Runtime_Value *key,
                                    const Om_Runtime_Value *value);
Om_Status om_runtime_map_get_value(const Om_Execution_Context *context,
                                   const void *map,
                                   const Om_Runtime_Value *key,
                                   Om_Runtime_Value *result);
Om_Status om_runtime_map_key_at(const Om_Execution_Context *context,
                                const void *map,
                                size_t index,
                                Om_Runtime_Value *result);
Om_Status om_runtime_map_length(const Om_Execution_Context *context,
                                const void *map,
                                size_t *result);

#endif
