#ifndef OMBASIC_RUNTIME_ABI_PROGRAM_DESCRIPTOR_H
#define OMBASIC_RUNTIME_ABI_PROGRAM_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/runtime/abi/type_descriptor.h"

typedef Om_Status (*Om_Runtime_Actor_State_Initializer)(
    Om_Execution_Context *context,
    void *state_payload);
typedef Om_Status (*Om_Runtime_Actor_Handler)(
    Om_Execution_Context *context,
    void *state_payload,
    const void *message_payload);

typedef struct Om_Runtime_Actor_Handler_Descriptor {
    const Om_Runtime_Type_Descriptor *message_type;
    Om_Runtime_Actor_Handler handler;
} Om_Runtime_Actor_Handler_Descriptor;

typedef struct Om_Runtime_Actor_Descriptor {
    uint64_t fingerprint;
    const char *canonical_name;
    const Om_Runtime_Type_Descriptor *state_type;
    Om_Runtime_Actor_State_Initializer initialize_state;
    const Om_Runtime_Actor_Handler_Descriptor *handlers;
    size_t handler_count;
} Om_Runtime_Actor_Descriptor;

typedef enum Om_Runtime_Actor_Down_Reason {
    OM_RUNTIME_ACTOR_DOWN_NORMAL = 0,
    OM_RUNTIME_ACTOR_DOWN_FAILED = 1,
    OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE = 2
} Om_Runtime_Actor_Down_Reason;

/* All descriptor arrays and callback pointers are borrowed immutable static
   generated data. Field offsets in type descriptors use native byte layout;
   fingerprints identify type, actor, program, and interface contracts. The
   source-span paths, entry function, actor handlers, and all referenced
   descriptors remain owned by the loaded program module. Keep that module
   loaded and those objects unchanged until its actor system and runtime
   context have stopped. */
typedef struct Om_Runtime_Program_Descriptor {
    uint32_t abi_version;
    uint64_t fingerprint;
    uint64_t interface_fingerprint;
    const Om_Runtime_Type_Descriptor *const *types;
    size_t type_count;
    const Om_Runtime_Actor_Descriptor *const *actors;
    size_t actor_count;
    const Om_Runtime_Type_Descriptor *actor_down_message_type;
    size_t actor_down_actor_id_offset;
    size_t actor_down_reason_offset;
    const Om_Runtime_Source_Span *source_spans;
    size_t source_span_count;
    Om_Runtime_Entry_Function entry_action;
    size_t collection_threshold;
} Om_Runtime_Program_Descriptor;

Om_Status om_runtime_validate_program(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program);

/* Source span storage is borrowed and must outlive the context. */
Om_Status om_runtime_set_source_spans(
    Om_Execution_Context *context,
    const Om_Runtime_Source_Span *spans,
    size_t span_count);

#endif
