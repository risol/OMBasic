#ifndef OMBASIC_RUNTIME_ABI_TYPE_DESCRIPTOR_H
#define OMBASIC_RUNTIME_ABI_TYPE_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/system_actors.h"
#include "ombasic/runtime/abi/base.h"

typedef enum Om_Runtime_Type_Kind {
    OM_RUNTIME_TYPE_UNIT = 0,
    OM_RUNTIME_TYPE_NULL,
    OM_RUNTIME_TYPE_BOOL,
    OM_RUNTIME_TYPE_INT,
    OM_RUNTIME_TYPE_FLOAT,
    OM_RUNTIME_TYPE_STRING,
    OM_RUNTIME_TYPE_RECORD,
    OM_RUNTIME_TYPE_MESSAGE,
    OM_RUNTIME_TYPE_LIST,
    OM_RUNTIME_TYPE_MAP,
    OM_RUNTIME_TYPE_RESULT,
    OM_RUNTIME_TYPE_NATIVE_POINTER,
    OM_RUNTIME_TYPE_ACTOR_REF
} Om_Runtime_Type_Kind;

typedef struct Om_Runtime_Field_Descriptor {
    size_t offset;
    const Om_Runtime_Type_Descriptor *type;
} Om_Runtime_Field_Descriptor;

typedef Om_Status (*Om_Runtime_Trace_Visitor)(void *managed_value,
                                             void *visitor_context);
typedef Om_Status (*Om_Runtime_Trace_Function)(
    Om_Execution_Context *context,
    const void *value,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context);
typedef Om_Status (*Om_Runtime_Copy_Function)(Om_Execution_Context *context,
                                             void *destination,
                                             const void *source);
typedef Om_Status (*Om_Runtime_Equal_Function)(Om_Execution_Context *context,
                                              const void *left,
                                              const void *right,
                                              bool *equal);
typedef Om_Status (*Om_Runtime_Hash_Function)(Om_Execution_Context *context,
                                             const void *value,
                                             uint64_t *hash);
typedef Om_Status (*Om_Runtime_Byte_Sink)(const uint8_t *bytes,
                                         size_t length,
                                         void *sink_context);
typedef Om_Status (*Om_Runtime_Serialize_Function)(
    Om_Execution_Context *context,
    const void *value,
    Om_Runtime_Byte_Sink sink,
    void *sink_context);

/* Generated descriptors are immutable static data owned by the native program
   module. The module must remain loaded while the runtime uses the program.
   size/alignment describe the native payload layout, field offsets are byte
   offsets in that layout, and fingerprints identify the serialized type
   contract. Trace/copy/equality/hash/serialization callbacks are borrowed
   function pointers and must remain callable for the same lifetime. */
struct Om_Runtime_Type_Descriptor {
    uint64_t fingerprint;
    Om_Runtime_Type_Kind kind;
    size_t size;
    size_t alignment;
    const Om_Runtime_Field_Descriptor *fields;
    size_t field_count;
    const Om_Runtime_Type_Descriptor *element_type;
    const Om_Runtime_Type_Descriptor *key_type;
    const Om_Runtime_Type_Descriptor *value_type;
    Om_Runtime_Trace_Function trace;
    Om_Runtime_Copy_Function copy;
    Om_Runtime_Equal_Function equal;
    Om_Runtime_Hash_Function hash;
    Om_Runtime_Serialize_Function serialize;
    Om_System_Message_Id system_message_id;
};

#endif
