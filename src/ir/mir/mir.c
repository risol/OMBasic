#include "ombasic/ir/mir.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/system_actors.h"

#define OM_MIR_MAX_TYPE_DEPTH ((size_t)128)
#define OM_MIR_MAX_BLOCKS_PER_FUNCTION ((size_t)4096)

static bool range_is_valid(size_t start, size_t count, size_t total);

typedef struct Om_Mir_Storage {
    Om_Vector functions;
    Om_Vector blocks;
    Om_Vector values;
    Om_Vector instructions;
    Om_Vector operands;
    Om_Vector edges;
    Om_Vector edge_arguments;
    Om_Vector nominals;
    Om_Vector fields;
    Om_Vector actors;
    Om_Vector actor_state_initializers;
    Om_Vector actor_handlers;
    Om_Vector string_data;
} Om_Mir_Storage;

static Om_Mir_Storage *storage_of(const Om_Mir_Project *project)
{
    return project == NULL ? NULL : project->owned_storage;
}

static void sync_project(Om_Mir_Project *project, Om_Mir_Storage *storage)
{
    project->functions = storage->functions.data;
    project->function_count = storage->functions.length;
    project->blocks = storage->blocks.data;
    project->block_count = storage->blocks.length;
    project->values = storage->values.data;
    project->value_count = storage->values.length;
    project->instructions = storage->instructions.data;
    project->instruction_count = storage->instructions.length;
    project->operands = storage->operands.data;
    project->operand_count = storage->operands.length;
    project->edges = storage->edges.data;
    project->edge_count = storage->edges.length;
    project->edge_arguments = storage->edge_arguments.data;
    project->edge_argument_count = storage->edge_arguments.length;
    project->nominals = storage->nominals.data;
    project->nominal_count = storage->nominals.length;
    project->fields = storage->fields.data;
    project->field_count = storage->fields.length;
    project->actors = storage->actors.data;
    project->actor_count = storage->actors.length;
    project->actor_state_initializers = storage->actor_state_initializers.data;
    project->actor_state_initializer_count =
        storage->actor_state_initializers.length;
    project->actor_handlers = storage->actor_handlers.data;
    project->actor_handler_count = storage->actor_handlers.length;
    project->string_data = storage->string_data.data;
    project->string_data_length = storage->string_data.length;
}

static void destroy_storage(Om_Mir_Storage *storage)
{
    if (storage == NULL) {
        return;
    }
    om_vector_destroy(&storage->edge_arguments);
    om_vector_destroy(&storage->fields);
    om_vector_destroy(&storage->nominals);
    om_vector_destroy(&storage->actor_handlers);
    om_vector_destroy(&storage->actor_state_initializers);
    om_vector_destroy(&storage->actors);
    om_vector_destroy(&storage->string_data);
    om_vector_destroy(&storage->edges);
    om_vector_destroy(&storage->operands);
    om_vector_destroy(&storage->instructions);
    om_vector_destroy(&storage->values);
    om_vector_destroy(&storage->blocks);
    om_vector_destroy(&storage->functions);
    om_free(storage);
}

static Om_Status init_storage(Om_Mir_Storage *storage)
{
    Om_Status status = om_vector_init(&storage->functions,
                                      sizeof(Om_Mir_Function));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->blocks, sizeof(Om_Mir_Block));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->values, sizeof(Om_Mir_Value));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->instructions,
                                sizeof(Om_Mir_Instruction));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->operands, sizeof(Om_Mir_Value_Id));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->edges, sizeof(Om_Mir_Edge));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->edge_arguments,
                                sizeof(Om_Mir_Value_Id));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->nominals,
                                sizeof(Om_Mir_Nominal_Descriptor));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->fields,
                                sizeof(Om_Mir_Field_Descriptor));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->actors,
                                sizeof(Om_Mir_Actor_Descriptor));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->actor_state_initializers,
            sizeof(Om_Mir_Actor_State_Initializer));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->actor_handlers,
                                sizeof(Om_Mir_Actor_Handler));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->string_data, sizeof(char));
    }
    return status;
}

static Om_Mir_Function *function_at(Om_Mir_Project *project,
                                    Om_Mir_Function_Id id)
{
    Om_Mir_Storage *storage = storage_of(project);
    return storage == NULL ? NULL : om_vector_at(&storage->functions, id.value);
}

static Om_Mir_Block *block_at(Om_Mir_Project *project, Om_Mir_Block_Id id)
{
    Om_Mir_Storage *storage = storage_of(project);
    return storage == NULL ? NULL : om_vector_at(&storage->blocks, id.value);
}

static const Om_Mir_Block *block_at_const(const Om_Mir_Project *project,
                                          Om_Mir_Block_Id id)
{
    Om_Mir_Storage *storage = storage_of(project);
    return storage == NULL ? NULL : om_vector_at_const(&storage->blocks, id.value);
}

static const Om_Mir_Value *value_at_const(const Om_Mir_Project *project,
                                          Om_Mir_Value_Id id)
{
    Om_Mir_Storage *storage = storage_of(project);
    return storage == NULL ? NULL : om_vector_at_const(&storage->values, id.value);
}

static Om_Status resolve_type(const Om_Type_Store *store,
                              Om_Type_Id type_id,
                              Om_Type_Id *resolved)
{
    if (store == NULL || resolved == NULL || type_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_type_store_resolve(store, type_id, resolved);
}

static Om_Status type_equal_recursive(const Om_Type_Store *store,
                                      Om_Type_Id left,
                                      Om_Type_Id right,
                                      size_t depth,
                                      bool *equal)
{
    if (equal == NULL || depth >= OM_MIR_MAX_TYPE_DEPTH) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved_left = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_right = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(store, left, &resolved_left);
    if (status == OM_STATUS_OK) {
        status = resolve_type(store, right, &resolved_right);
    }
    Om_Type_View left_view = {0};
    Om_Type_View right_view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved_left, &left_view);
    }
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved_right, &right_view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (left_view.kind != right_view.kind
        || left_view.child_count != right_view.child_count) {
        *equal = false;
        return OM_STATUS_OK;
    }
    if (left_view.kind == OM_TYPE_VARIABLE) {
        *equal = resolved_left.value == resolved_right.value;
        return OM_STATUS_OK;
    }
    if ((left_view.kind == OM_TYPE_RECORD
         || left_view.kind == OM_TYPE_MESSAGE
         || left_view.kind == OM_TYPE_ACTOR_PROTOCOL)
        && left_view.nominal_symbol.value != right_view.nominal_symbol.value) {
        *equal = false;
        return OM_STATUS_OK;
    }
    *equal = true;
    for (size_t index = 0; status == OM_STATUS_OK && *equal
         && index < left_view.child_count; ++index) {
        Om_Type_Id left_child = OM_TYPE_ID_INVALID;
        Om_Type_Id right_child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(store, resolved_left, index,
                                        &left_child);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(store, resolved_right, index,
                                            &right_child);
        }
        if (status == OM_STATUS_OK) {
            status = type_equal_recursive(store, left_child, right_child,
                                          depth + 1, equal);
        }
    }
    return status;
}

static Om_Status type_equal(const Om_Type_Store *store,
                            Om_Type_Id left,
                            Om_Type_Id right,
                            bool *equal)
{
    return type_equal_recursive(store, left, right, 0, equal);
}

static Om_Status type_is_concrete(const Om_Type_Store *store,
                                  Om_Type_Id type_id,
                                  size_t depth,
                                  bool *concrete)
{
    if (depth >= OM_MIR_MAX_TYPE_DEPTH || concrete == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(store, type_id, &resolved);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved, &view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (view.kind == OM_TYPE_VARIABLE) {
        *concrete = false;
        return OM_STATUS_OK;
    }
    *concrete = true;
    for (size_t index = 0; *concrete && index < view.child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(store, resolved, index, &child);
        if (status == OM_STATUS_OK) {
            status = type_is_concrete(store, child, depth + 1, concrete);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

Om_Status om_mir_project_init(Om_Mir_Project *project,
                              const Om_Type_Store *type_store)
{
    if (project == NULL || type_store == NULL || project->owned_storage != NULL
        || project->functions != NULL || project->blocks != NULL
        || project->values != NULL || project->instructions != NULL
        || project->nominals != NULL || project->fields != NULL
        || project->string_data != NULL || project->string_data_length != 0
        || project->function_count != 0 || project->block_count != 0
        || project->value_count != 0 || project->instruction_count != 0
        || project->operand_count != 0 || project->edge_count != 0
        || project->edge_argument_count != 0 || project->nominal_count != 0
        || project->field_count != 0 || project->actors != NULL
        || project->actor_count != 0
        || project->actor_state_initializers != NULL
        || project->actor_state_initializer_count != 0
        || project->actor_handlers != NULL || project->actor_handler_count != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Mir_Storage){0};
    Om_Status status = init_storage(storage);
    if (status != OM_STATUS_OK) {
        destroy_storage(storage);
        return status;
    }
    *project = (Om_Mir_Project){
        .type_store = type_store,
        .actor_down_message_symbol_id = OM_SYMBOL_ID_INVALID,
        .actor_down_message_type_id = OM_TYPE_ID_INVALID,
        .owned_storage = storage,
    };
    sync_project(project, storage);
    return OM_STATUS_OK;
}

void om_mir_project_destroy(Om_Mir_Project *project)
{
    if (project == NULL) {
        return;
    }
    destroy_storage(storage_of(project));
    *project = (Om_Mir_Project){0};
}

Om_Status om_mir_add_nominal_descriptor(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_Type_Kind type_kind,
    const Om_Mir_Field_Descriptor *fields,
    size_t field_count)
{
    Om_Mir_Storage *storage = storage_of(project);
    if (storage == NULL || symbol_id.value == UINT32_MAX
        || (type_kind != OM_TYPE_RECORD && type_kind != OM_TYPE_MESSAGE)
        || (field_count != 0 && fields == NULL)
        || storage->nominals.length >= UINT32_MAX
        || storage->fields.length > UINT32_MAX
        || field_count > UINT32_MAX - storage->fields.length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < storage->nominals.length; ++index) {
        const Om_Mir_Nominal_Descriptor *existing = om_vector_at_const(
            &storage->nominals, index);
        if (existing == NULL || existing->symbol_id.value == symbol_id.value) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    for (size_t index = 0; index < field_count; ++index) {
        if (fields[index].symbol_id.value == UINT32_MAX
            || fields[index].type_id.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t other = index + 1; other < field_count; ++other) {
            if (fields[index].symbol_id.value
                == fields[other].symbol_id.value) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    const size_t old_field_count = storage->fields.length;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < field_count;
         ++index) {
        status = om_vector_push(&storage->fields, &fields[index]);
    }
    if (status == OM_STATUS_OK) {
        const Om_Mir_Nominal_Descriptor descriptor = {
            .symbol_id = symbol_id,
            .type_kind = type_kind,
            .field_start = (uint32_t)old_field_count,
            .field_count = (uint32_t)field_count,
        };
        status = om_vector_push(&storage->nominals, &descriptor);
    }
    if (status == OM_STATUS_OK) {
        sync_project(project, storage);
    } else {
        storage->fields.length = old_field_count;
        sync_project(project, storage);
    }
    return status;
}

Om_Status om_mir_set_system_message_id(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_System_Message_Id system_message_id)
{
    Om_Mir_Storage *storage = storage_of(project);
    if (storage == NULL || symbol_id.value == UINT32_MAX
        || (system_message_id != OM_SYSTEM_MESSAGE_ID_NONE
            && (system_message_id > OM_SYSTEM_MESSAGE_ID_UDP_CLOSE_COMPLETED
                && system_message_id != OM_SYSTEM_MESSAGE_ID_ANY))) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < storage->nominals.length; ++index) {
        Om_Mir_Nominal_Descriptor *descriptor = om_vector_at(
            &storage->nominals, index);
        if (descriptor != NULL
            && descriptor->symbol_id.value == symbol_id.value) {
            descriptor->system_message_id = system_message_id;
            sync_project(project, storage);
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}

Om_Status om_mir_set_nominal_identity(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_String_View canonical_module_path,
    Om_String_View name)
{
    Om_Mir_Storage *storage = storage_of(project);
    if (storage == NULL || symbol_id.value == UINT32_MAX
        || (canonical_module_path.length != 0
            && canonical_module_path.data == NULL)
        || name.data == NULL || name.length == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < storage->nominals.length; ++index) {
        Om_Mir_Nominal_Descriptor *descriptor = om_vector_at(
            &storage->nominals, index);
        if (descriptor != NULL
            && descriptor->symbol_id.value == symbol_id.value) {
            descriptor->canonical_module_path = canonical_module_path;
            descriptor->name = name;
            sync_project(project, storage);
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}

Om_Status om_mir_add_actor_descriptor(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_Type_Id state_type_id,
    const Om_Mir_Actor_State_Initializer *initializers,
    size_t initializer_count,
    const Om_Mir_Actor_Handler *handlers,
    size_t handler_count)
{
    Om_Mir_Storage *storage = storage_of(project);
    if (storage == NULL || symbol_id.value == UINT32_MAX
        || state_type_id.value == UINT32_MAX
        || (initializer_count != 0 && initializers == NULL)
        || (handler_count != 0 && handlers == NULL)
        || storage->actors.length >= UINT32_MAX
        || storage->actor_state_initializers.length > UINT32_MAX
        || initializer_count > UINT32_MAX
            - storage->actor_state_initializers.length
        || storage->actor_handlers.length > UINT32_MAX
        || handler_count > UINT32_MAX - storage->actor_handlers.length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < storage->actors.length; ++index) {
        const Om_Mir_Actor_Descriptor *existing = om_vector_at_const(
            &storage->actors, index);
        if (existing == NULL || existing->symbol_id.value == symbol_id.value) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    for (size_t index = 0; index < initializer_count; ++index) {
        if (initializers[index].field_symbol_id.value == UINT32_MAX
            || initializers[index].function_instance_id.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t other = index + 1; other < initializer_count; ++other) {
            if (initializers[index].field_symbol_id.value
                == initializers[other].field_symbol_id.value) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    for (size_t index = 0; index < handler_count; ++index) {
        if (handlers[index].message_symbol_id.value == UINT32_MAX
            || handlers[index].message_type_id.value == UINT32_MAX
            || handlers[index].function_instance_id.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t other = index + 1; other < handler_count; ++other) {
            if (handlers[index].message_symbol_id.value
                == handlers[other].message_symbol_id.value) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    const size_t old_initializers = storage->actor_state_initializers.length;
    const size_t old_handlers = storage->actor_handlers.length;
    Om_Status status = om_vector_reserve(&storage->actor_state_initializers,
        old_initializers + initializer_count);
    if (status == OM_STATUS_OK) {
        status = om_vector_reserve(&storage->actor_handlers,
            old_handlers + handler_count);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < initializer_count; ++index) {
        status = om_vector_push(&storage->actor_state_initializers,
                                &initializers[index]);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < handler_count;
         ++index) {
        status = om_vector_push(&storage->actor_handlers, &handlers[index]);
    }
    if (status == OM_STATUS_OK) {
        const Om_Mir_Actor_Descriptor descriptor = {
            .symbol_id = symbol_id,
            .state_type_id = state_type_id,
            .state_initializer_start = (uint32_t)old_initializers,
            .state_initializer_count = (uint32_t)initializer_count,
            .handler_start = (uint32_t)old_handlers,
            .handler_count = (uint32_t)handler_count,
        };
        status = om_vector_push(&storage->actors, &descriptor);
    }
    if (status != OM_STATUS_OK) {
        storage->actor_state_initializers.length = old_initializers;
        storage->actor_handlers.length = old_handlers;
    }
    sync_project(project, storage);
    return status;
}

Om_Status om_mir_set_actor_identity(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_String_View canonical_module_path,
    Om_String_View name)
{
    Om_Mir_Storage *storage = storage_of(project);
    if (storage == NULL || symbol_id.value == UINT32_MAX
        || (canonical_module_path.length != 0
            && canonical_module_path.data == NULL)
        || name.data == NULL || name.length == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < storage->actors.length; ++index) {
        Om_Mir_Actor_Descriptor *descriptor = om_vector_at(
            &storage->actors, index);
        if (descriptor != NULL
            && descriptor->symbol_id.value == symbol_id.value) {
            descriptor->canonical_module_path = canonical_module_path;
            descriptor->name = name;
            sync_project(project, storage);
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}

Om_Status om_mir_add_function(Om_Mir_Project *project,
                              Om_Function_Instance_Id instance_id,
                              Om_Mir_Function_Kind kind,
                              Om_Module_Id module_id,
                              Om_Symbol_Id symbol_id,
                              Om_Type_Id signature_type_id,
                              uint32_t effects,
                              Om_Source_Span span,
                              Om_Mir_Function_Id *function_id)
{
    Om_Mir_Storage *storage = storage_of(project);
    if (storage == NULL || function_id == NULL
        || (size_t)kind > (size_t)OM_MIR_ACTOR_STATE_INIT
        || storage->functions.length >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Mir_Function function = {
        .id = {(uint32_t)storage->functions.length},
        .instance_id = instance_id,
        .kind = kind,
        .module_id = module_id,
        .symbol_id = symbol_id,
        .signature_type_id = signature_type_id,
        .entry_block_id = OM_MIR_BLOCK_ID_INVALID,
        .effects = effects,
        .span = span,
    };
    const Om_Status status = om_vector_push(&storage->functions, &function);
    if (status == OM_STATUS_OK) {
        *function_id = function.id;
        sync_project(project, storage);
    }
    return status;
}

Om_Status om_mir_set_function_debug_name(Om_Mir_Project *project,
                                         Om_Mir_Function_Id function_id,
                                         Om_String_View name)
{
    Om_Mir_Function *function = function_at(project, function_id);
    if (function == NULL || (name.length != 0 && name.data == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    function->debug_name = name;
    return OM_STATUS_OK;
}

Om_Status om_mir_add_block(Om_Mir_Project *project,
                           Om_Mir_Function_Id function_id,
                           Om_Mir_Block_Id *block_id)
{
    Om_Mir_Storage *storage = storage_of(project);
    Om_Mir_Function *function = function_at(project, function_id);
    if (storage == NULL || function == NULL || block_id == NULL
        || storage->blocks.length >= UINT32_MAX
        || storage->values.length > UINT32_MAX
        || storage->instructions.length > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Mir_Block block = {
        .id = {(uint32_t)storage->blocks.length},
        .function_id = function_id,
        .parameter_start = (uint32_t)storage->values.length,
        .parameter_count = 0,
        .instruction_start = (uint32_t)storage->instructions.length,
        .instruction_count = 0,
        .terminator = {
            .kind = OM_MIR_TERMINATOR_NONE,
            .value_id = OM_MIR_VALUE_ID_INVALID,
            .edge_start = 0,
            .edge_count = 0,
        },
        .has_terminator = false,
    };
    const Om_Status status = om_vector_push(&storage->blocks, &block);
    if (status == OM_STATUS_OK) {
        if (function->entry_block_id.value == UINT32_MAX) {
            function->entry_block_id = block.id;
        }
        *block_id = block.id;
        sync_project(project, storage);
    }
    return status;
}

Om_Status om_mir_add_block_parameter(Om_Mir_Project *project,
                                     Om_Mir_Block_Id block_id,
                                     Om_Type_Id type_id,
                                     Om_Mir_Value_Id *value_id)
{
    Om_Mir_Storage *storage = storage_of(project);
    Om_Mir_Block *block = block_at(project, block_id);
    if (storage == NULL || block == NULL || value_id == NULL
        || block->has_terminator || block->instruction_count != 0
        || block->parameter_start > storage->values.length
        || block->parameter_count
            != storage->values.length - block->parameter_start
        || storage->values.length >= UINT32_MAX
        || block->parameter_count >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Mir_Value value = {
        .id = {(uint32_t)storage->values.length},
        .type_id = type_id,
        .defining_block_id = block_id,
        .defining_instruction_id = OM_MIR_INSTRUCTION_ID_INVALID,
        .is_block_parameter = true,
    };
    const Om_Status status = om_vector_push(&storage->values, &value);
    if (status == OM_STATUS_OK) {
        ++block->parameter_count;
        *value_id = value.id;
        sync_project(project, storage);
    }
    return status;
}

Om_Status om_mir_add_instruction(Om_Mir_Project *project,
                                 Om_Mir_Block_Id block_id,
                                 const Om_Mir_Instruction_Spec *spec,
                                 const Om_Mir_Value_Id *operands,
                                 size_t operand_count,
                                 Om_Mir_Value_Id *result_id)
{
    Om_Mir_Storage *storage = storage_of(project);
    Om_Mir_Block *block = block_at(project, block_id);
    if (storage == NULL || block == NULL || spec == NULL || result_id == NULL
        || (operand_count != 0 && operands == NULL)
        || (spec->string_value.length != 0 && spec->string_value.data == NULL)
        || block->has_terminator
        || block->instruction_start > storage->instructions.length
        || block->instruction_count
            != storage->instructions.length - block->instruction_start
        || storage->values.length >= UINT32_MAX
        || storage->instructions.length >= UINT32_MAX
        || storage->operands.length > UINT32_MAX
        || operand_count > UINT32_MAX - storage->operands.length
        || storage->string_data.length > UINT32_MAX
        || spec->string_value.length > UINT32_MAX - storage->string_data.length
        || block->instruction_count >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t old_value_count = storage->values.length;
    const size_t old_instruction_count = storage->instructions.length;
    const size_t old_operand_count = storage->operands.length;
    const size_t old_string_data_length = storage->string_data.length;
    Om_Status status = OM_STATUS_OK;
    char *string_value_copy = NULL;
    if (spec->string_value.length != 0) {
        string_value_copy = om_alloc_array(spec->string_value.length,
                                           sizeof(*string_value_copy));
        if (string_value_copy == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memcpy(string_value_copy, spec->string_value.data,
                     spec->string_value.length);
        status = om_vector_reserve(&storage->string_data,
            old_string_data_length + spec->string_value.length);
        if (status == OM_STATUS_OK) {
            (void)memcpy((char *)storage->string_data.data
                    + old_string_data_length,
                string_value_copy, spec->string_value.length);
            storage->string_data.length = old_string_data_length
                + spec->string_value.length;
        }
    }
    om_free(string_value_copy);
    for (size_t index = 0; status == OM_STATUS_OK && index < operand_count;
         ++index) {
        status = om_vector_push(&storage->operands, &operands[index]);
    }
    const Om_Mir_Value_Id new_value_id = {
        (uint32_t)old_value_count,
    };
    const Om_Mir_Instruction_Id instruction_id = {
        (uint32_t)old_instruction_count,
    };
    const Om_Mir_Value value = {
        .id = new_value_id,
        .type_id = spec->type_id,
        .defining_block_id = block_id,
        .defining_instruction_id = instruction_id,
        .is_block_parameter = false,
    };
    if (status == OM_STATUS_OK) {
        status = om_vector_push(&storage->values, &value);
    }
    const Om_Mir_Instruction instruction = {
        .id = instruction_id,
        .block_id = block_id,
        .result_id = new_value_id,
        .opcode = spec->opcode,
        .operator_kind = spec->operator_kind,
        .callee_instance_id = spec->callee_instance_id,
        .symbol_id = spec->symbol_id,
        .span = spec->span,
        .operand_start = (uint32_t)old_operand_count,
        .operand_count = (uint32_t)operand_count,
        .effects = spec->effects,
        .flags = spec->flags,
        .integer_value = spec->integer_value,
        .float_value = spec->float_value,
        .string_start = (uint32_t)old_string_data_length,
        .string_length = (uint32_t)spec->string_value.length,
    };
    if (status == OM_STATUS_OK) {
        status = om_vector_push(&storage->instructions, &instruction);
    }
    if (status != OM_STATUS_OK) {
        storage->values.length = old_value_count;
        storage->instructions.length = old_instruction_count;
        storage->operands.length = old_operand_count;
        storage->string_data.length = old_string_data_length;
        sync_project(project, storage);
        return status;
    }
    ++block->instruction_count;
    *result_id = new_value_id;
    sync_project(project, storage);
    return OM_STATUS_OK;
}

Om_Status om_mir_set_terminator(Om_Mir_Project *project,
                                Om_Mir_Block_Id block_id,
                                Om_Mir_Terminator_Kind kind,
                                Om_Mir_Value_Id value_id,
                                const Om_Mir_Edge_Input *edges,
                                size_t edge_count)
{
    Om_Mir_Storage *storage = storage_of(project);
    Om_Mir_Block *block = block_at(project, block_id);
    if (storage == NULL || block == NULL
        || (size_t)kind > (size_t)OM_MIR_UNREACHABLE
        || kind == OM_MIR_TERMINATOR_NONE
        || block->has_terminator
        || (edge_count != 0 && edges == NULL)
        || storage->edges.length > UINT32_MAX
        || edge_count > UINT32_MAX - storage->edges.length
        || storage->edge_arguments.length > UINT32_MAX
        || edge_count > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t total_argument_count = 0;
    for (size_t index = 0; index < edge_count; ++index) {
        if (edges[index].argument_count != 0 && edges[index].arguments == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (edges[index].argument_count > SIZE_MAX - total_argument_count) {
            return OM_STATUS_OVERFLOW;
        }
        total_argument_count += edges[index].argument_count;
    }
    if (total_argument_count > UINT32_MAX - storage->edge_arguments.length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t old_edge_count = storage->edges.length;
    const size_t old_argument_count = storage->edge_arguments.length;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < edge_count;
         ++index) {
        const Om_Mir_Edge_Input *input = &edges[index];
        const Om_Mir_Edge edge = {
            .target_block_id = input->target_block_id,
            .argument_start = (uint32_t)storage->edge_arguments.length,
            .argument_count = (uint32_t)input->argument_count,
            .has_case_value = input->has_case_value,
            .is_default_case = input->is_default_case,
            .case_value = input->case_value,
        };
        for (size_t argument = 0; status == OM_STATUS_OK
             && argument < input->argument_count; ++argument) {
            status = om_vector_push(&storage->edge_arguments,
                                    &input->arguments[argument]);
        }
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&storage->edges, &edge);
        }
    }
    if (status != OM_STATUS_OK) {
        storage->edges.length = old_edge_count;
        storage->edge_arguments.length = old_argument_count;
        return status;
    }
    block->terminator = (Om_Mir_Terminator){
        .kind = kind,
        .value_id = value_id,
        .edge_start = (uint32_t)old_edge_count,
        .edge_count = (uint32_t)edge_count,
    };
    block->has_terminator = true;
    sync_project(project, storage);
    return OM_STATUS_OK;
}

static Om_Status add_diagnostic(Om_Vector *diagnostics,
                                Om_Mir_Diagnostic_Code code,
                                Om_Mir_Function_Id function_id,
                                Om_Mir_Block_Id block_id,
                                Om_Mir_Value_Id value_id,
                                Om_Source_Span span)
{
    const Om_Mir_Diagnostic diagnostic = {
        .code = code,
        .function_id = function_id,
        .block_id = block_id,
        .value_id = value_id,
        .span = span,
    };
    return om_vector_push(diagnostics, &diagnostic);
}

static Om_Status verify_value_type(const Om_Mir_Project *project,
                                   Om_Vector *diagnostics,
                                   Om_Mir_Function_Id function_id,
                                   Om_Mir_Block_Id block_id,
                                   Om_Mir_Value_Id value_id,
                                   Om_Type_Id expected,
                                   Om_Source_Span span)
{
    const Om_Mir_Value *value = value_at_const(project, value_id);
    if (value == NULL) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                              function_id, block_id, value_id, span);
    }
    bool equal = false;
    const Om_Status status = type_equal(project->type_store, value->type_id,
                                        expected, &equal);
    if (status != OM_STATUS_OK || !equal) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                              function_id, block_id, value_id, span);
    }
    return OM_STATUS_OK;
}

static Om_Status value_type_kind(const Om_Mir_Project *project,
                                 Om_Mir_Value_Id value_id,
                                 Om_Type_Kind *kind)
{
    const Om_Mir_Value *value = value_at_const(project, value_id);
    if (value == NULL || kind == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(project->type_store, value->type_id,
                                    &resolved);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(project->type_store, resolved, &view);
    }
    if (status == OM_STATUS_OK) {
        *kind = view.kind;
    }
    return status;
}

static Om_Status type_supports_compare(const Om_Type_Store *store,
                                      Om_Type_Id type_id,
                                      bool ordered,
                                      size_t depth,
                                      bool *supported)
{
    if (store == NULL || supported == NULL || depth >= 512) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_resolve(store, type_id, &resolved_type);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved_type, &view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (ordered) {
        *supported = view.kind == OM_TYPE_INT || view.kind == OM_TYPE_FLOAT
            || view.kind == OM_TYPE_STRING;
        return OM_STATUS_OK;
    }
    *supported = view.kind == OM_TYPE_INT || view.kind == OM_TYPE_FLOAT
        || view.kind == OM_TYPE_BOOL || view.kind == OM_TYPE_STRING
        || view.kind == OM_TYPE_NULL || view.kind == OM_TYPE_ACTOR_REF
        || view.kind == OM_TYPE_ACTOR_ID || view.kind == OM_TYPE_DOWN_REASON;
    if (view.kind == OM_TYPE_RESULT && view.child_count == 2) {
        *supported = true;
        for (size_t index = 0; status == OM_STATUS_OK && *supported
             && index < view.child_count; ++index) {
            Om_Type_Id child_type = OM_TYPE_ID_INVALID;
            bool child_supported = false;
            status = om_type_store_child_at(store, resolved_type, index,
                                            &child_type);
            if (status == OM_STATUS_OK) {
                status = type_supports_compare(store, child_type, false,
                    depth + 1, &child_supported);
            }
            *supported = child_supported;
        }
    }
    return status;
}

static const Om_Mir_Nominal_Descriptor *nominal_for_symbol(
    const Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_Type_Kind type_kind)
{
    for (size_t index = 0; index < project->nominal_count; ++index) {
        const Om_Mir_Nominal_Descriptor *nominal = &project->nominals[index];
        if (nominal->symbol_id.value == symbol_id.value
            && nominal->type_kind == type_kind) {
            return nominal;
        }
    }
    return NULL;
}

static const Om_Mir_Field_Descriptor *field_for_symbol(
    const Om_Mir_Project *project,
    const Om_Mir_Nominal_Descriptor *nominal,
    Om_Symbol_Id symbol_id)
{
    if (nominal == NULL
        || !range_is_valid(nominal->field_start, nominal->field_count,
                           project->field_count)) {
        return NULL;
    }
    for (size_t index = 0; index < nominal->field_count; ++index) {
        const Om_Mir_Field_Descriptor *field = &project->fields[
            nominal->field_start + index];
        if (field->symbol_id.value == symbol_id.value) {
            return field;
        }
    }
    return NULL;
}

static const Om_Mir_Function *function_for_instance(
    const Om_Mir_Project *project,
    Om_Function_Instance_Id instance_id)
{
    for (size_t index = 0; index < project->function_count; ++index) {
        const Om_Mir_Function *function = &project->functions[index];
        if (function->instance_id.value == instance_id.value) {
            return function;
        }
    }
    return NULL;
}

static Om_Status verify_instruction(const Om_Mir_Project *project,
                                    Om_Vector *diagnostics,
                                    const Om_Mir_Function *function,
                                    const Om_Mir_Block *block,
                                    const Om_Mir_Instruction *instruction)
{
    if (instruction->operand_start > project->operand_count
        || instruction->operand_count
            > project->operand_count - instruction->operand_start
        || !range_is_valid(instruction->string_start,
                           instruction->string_length,
                           project->string_data_length)) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                              function->id, block->id, instruction->result_id,
                              instruction->span);
    }
    const Om_Mir_Value *result = value_at_const(project,
                                                instruction->result_id);
    if (result == NULL || result->type_id.value == UINT32_MAX) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                              function->id, block->id, instruction->result_id,
                              instruction->span);
    }
    bool concrete = false;
    Om_Status status = type_is_concrete(project->type_store, result->type_id,
                                        0, &concrete);
    if (status != OM_STATUS_OK || !concrete) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1105,
                              function->id, block->id, result->id,
                              instruction->span);
    }
    const size_t operand_count = instruction->operand_count;
    const Om_Mir_Value_Id *operands = operand_count == 0
        ? NULL : project->operands + instruction->operand_start;
    Om_Type_Kind result_kind = OM_TYPE_VARIABLE;
    status = value_type_kind(project, result->id, &result_kind);
    if (status != OM_STATUS_OK) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                              function->id, block->id, result->id,
                              instruction->span);
    }
#define MIR_EXPECT_OPERANDS(count_) \
    do { \
        if (operand_count != (size_t)(count_)) { \
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101, \
                function->id, block->id, result->id, instruction->span); \
        } \
    } while (false)
#define MIR_EXPECT_RESULT(kind_) \
    do { \
        if (result_kind != (kind_)) { \
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102, \
                function->id, block->id, result->id, instruction->span); \
        } \
    } while (false)
    switch (instruction->opcode) {
    case OM_MIR_CONST_INT:
        MIR_EXPECT_OPERANDS(0);
        if (result_kind == OM_TYPE_DOWN_REASON) {
            Om_Type_View reason_type = {0};
            status = om_type_store_get(project->type_store, result->type_id,
                                       &reason_type);
            if (status != OM_STATUS_OK
                || reason_type.nominal_symbol.value == UINT32_MAX
                || instruction->integer_value > 2) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                    function->id, block->id, result->id, instruction->span);
            }
        } else if (result_kind != OM_TYPE_INT) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    case OM_MIR_CONST_FLOAT:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_FLOAT);
        break;
    case OM_MIR_CONST_BOOL:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_BOOL);
        break;
    case OM_MIR_CONST_NULL:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_NULL);
        break;
    case OM_MIR_RUNTIME_STRING_CREATE:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_STRING);
        break;
    case OM_MIR_RUNTIME_PRINT:
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_UNIT);
        {
            Om_Type_Kind input_kind = OM_TYPE_VARIABLE;
            status = value_type_kind(project, operands[0], &input_kind);
            if (status != OM_STATUS_OK || input_kind != OM_TYPE_STRING) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                    function->id, block->id, operands[0], instruction->span);
            }
        }
        break;
    case OM_MIR_RUNTIME_STRING_FORMAT: {
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_STRING);
        Om_Type_Kind input_kind = OM_TYPE_VARIABLE;
        status = value_type_kind(project, operands[0], &input_kind);
        if (status != OM_STATUS_OK
            || (input_kind != OM_TYPE_INT && input_kind != OM_TYPE_FLOAT
                && input_kind != OM_TYPE_BOOL && input_kind != OM_TYPE_STRING
                && input_kind != OM_TYPE_NULL)) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        break;
    }
    case OM_MIR_RUNTIME_STRING_CONCAT:
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_STRING);
        for (size_t index = 0; index < operand_count; ++index) {
            Om_Type_Kind input_kind = OM_TYPE_VARIABLE;
            status = value_type_kind(project, operands[index], &input_kind);
            if (status != OM_STATUS_OK || input_kind != OM_TYPE_STRING) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                    function->id, block->id, operands[index], instruction->span);
            }
        }
        break;
    case OM_MIR_LIST_CREATE: {
        MIR_EXPECT_RESULT(OM_TYPE_LIST);
        Om_Type_Id element_type = OM_TYPE_ID_INVALID;
        Om_Type_View list_type = {0};
        status = resolve_type(project->type_store, result->type_id,
                              &element_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, element_type,
                                       &list_type);
        }
        if (status != OM_STATUS_OK || list_type.child_count != 1) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        status = om_type_store_child_at(project->type_store, element_type, 0,
                                        &element_type);
        for (size_t index = 0; status == OM_STATUS_OK
             && index < operand_count; ++index) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[index], element_type, instruction->span);
        }
        return status;
    }
    case OM_MIR_LIST_CONCAT: {
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_LIST);
        Om_Type_Id result_type = OM_TYPE_ID_INVALID;
        status = resolve_type(project->type_store, result->type_id,
                              &result_type);
        for (size_t index = 0; status == OM_STATUS_OK && index < 2; ++index) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[index], result_type, instruction->span);
        }
        return status;
    }
    case OM_MIR_LIST_LENGTH: {
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_INT);
        const Om_Mir_Value *list = value_at_const(project, operands[0]);
        Om_Type_Id list_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View list_type = {0};
        status = list == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, list->type_id, &list_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, list_type_id,
                                       &list_type);
        }
        if (status != OM_STATUS_OK || list_type.kind != OM_TYPE_LIST
            || list_type.child_count != 1) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        break;
    }
    case OM_MIR_LIST_GET: {
        MIR_EXPECT_OPERANDS(2);
        const Om_Mir_Value *list = value_at_const(project, operands[0]);
        Om_Type_Id list_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View list_type = {0};
        status = list == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, list->type_id, &list_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, list_type_id,
                                       &list_type);
        }
        if (status != OM_STATUS_OK || list_type.kind != OM_TYPE_LIST
            || list_type.child_count != 1) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        Om_Type_Id element_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, list_type_id, 0,
                                        &element_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, result->id, element_type, instruction->span);
        }
        Om_Type_Id int_type = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = om_type_store_primitive(project->type_store, OM_TYPE_INT,
                                             &int_type);
        }
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[1], int_type, instruction->span);
        }
        return status;
    }
    case OM_MIR_MAP_LENGTH: {
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_INT);
        const Om_Mir_Value *map = value_at_const(project, operands[0]);
        Om_Type_Id map_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View map_type = {0};
        status = map == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, map->type_id, &map_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, map_type_id,
                                       &map_type);
        }
        if (status != OM_STATUS_OK || map_type.kind != OM_TYPE_MAP
            || map_type.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        break;
    }
    case OM_MIR_MAP_KEY_AT: {
        MIR_EXPECT_OPERANDS(2);
        const Om_Mir_Value *map = value_at_const(project, operands[0]);
        Om_Type_Id map_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View map_type = {0};
        status = map == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, map->type_id, &map_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, map_type_id,
                                       &map_type);
        }
        if (status != OM_STATUS_OK || map_type.kind != OM_TYPE_MAP
            || map_type.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        Om_Type_Id key_type = OM_TYPE_ID_INVALID;
        Om_Type_Id int_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, map_type_id, 0,
                                        &key_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, result->id, key_type, instruction->span);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_primitive(project->type_store, OM_TYPE_INT,
                                             &int_type);
        }
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[1], int_type, instruction->span);
        }
        return status;
    }
    case OM_MIR_MAP_CREATE: {
        MIR_EXPECT_RESULT(OM_TYPE_MAP);
        if (operand_count % 2 != 0) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id map_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View map_type = {0};
        status = resolve_type(project->type_store, result->type_id,
                              &map_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, map_type_id,
                                       &map_type);
        }
        if (status != OM_STATUS_OK || map_type.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id key_type = OM_TYPE_ID_INVALID;
        Om_Type_Id value_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, map_type_id, 0,
                                        &key_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(project->type_store, map_type_id, 1,
                                            &value_type);
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < operand_count; index += 2) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[index], key_type, instruction->span);
            if (status == OM_STATUS_OK) {
                status = verify_value_type(project, diagnostics, function->id,
                    block->id, operands[index + 1], value_type,
                    instruction->span);
            }
        }
        return status;
    }
    case OM_MIR_MAP_GET: {
        MIR_EXPECT_OPERANDS(2);
        const Om_Mir_Value *map = value_at_const(project, operands[0]);
        Om_Type_Id map_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View map_type = {0};
        status = map == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, map->type_id, &map_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, map_type_id,
                                       &map_type);
        }
        if (status != OM_STATUS_OK || map_type.kind != OM_TYPE_MAP
            || map_type.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        Om_Type_Id key_type = OM_TYPE_ID_INVALID;
        Om_Type_Id value_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, map_type_id, 0,
                                        &key_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(project->type_store, map_type_id, 1,
                                            &value_type);
        }
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[1], key_type, instruction->span);
        }
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, result->id, value_type, instruction->span);
        }
        return status;
    }
    case OM_MIR_RESULT_MAKE_OK:
    case OM_MIR_RESULT_MAKE_ERROR: {
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_RESULT);
        Om_Type_Id result_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View result_type = {0};
        status = resolve_type(project->type_store, result->type_id,
                              &result_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, result_type_id,
                                       &result_type);
        }
        if (status != OM_STATUS_OK || result_type.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id payload_type = OM_TYPE_ID_INVALID;
        const size_t payload_index = instruction->opcode == OM_MIR_RESULT_MAKE_OK
            ? 0 : 1;
        status = om_type_store_child_at(project->type_store, result_type_id,
                                        payload_index, &payload_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[0], payload_type, instruction->span);
        }
        return status;
    }
    case OM_MIR_RESULT_TAG:
    case OM_MIR_RESULT_OK_PAYLOAD:
    case OM_MIR_RESULT_ERROR_PAYLOAD: {
        MIR_EXPECT_OPERANDS(1);
        const Om_Mir_Value *input = value_at_const(project, operands[0]);
        Om_Type_Id input_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View input_type = {0};
        status = input == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, input->type_id, &input_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, input_type_id,
                                       &input_type);
        }
        if (status != OM_STATUS_OK || input_type.kind != OM_TYPE_RESULT
            || input_type.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        if (instruction->opcode == OM_MIR_RESULT_TAG) {
            MIR_EXPECT_RESULT(OM_TYPE_BOOL);
            break;
        }
        Om_Type_Id payload_type = OM_TYPE_ID_INVALID;
        const size_t payload_index = instruction->opcode
                == OM_MIR_RESULT_OK_PAYLOAD ? 0 : 1;
        status = om_type_store_child_at(project->type_store, input_type_id,
                                        payload_index, &payload_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, result->id, payload_type, instruction->span);
        }
        return status;
    }
    case OM_MIR_ALLOC_OBJECT: {
        Om_Type_Id object_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View object_type = {0};
        status = resolve_type(project->type_store, result->type_id,
                              &object_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, object_type_id,
                                       &object_type);
        }
        const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
            ? nominal_for_symbol(project, instruction->symbol_id,
                                 object_type.kind) : NULL;
        if (status != OM_STATUS_OK || nominal == NULL
            || object_type.nominal_symbol.value != instruction->symbol_id.value) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        if (operand_count != nominal->field_count) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        for (size_t index = 0; index < operand_count; ++index) {
            const Om_Mir_Field_Descriptor *field = &project->fields[
                nominal->field_start + index];
            Om_Type_Kind operand_kind = OM_TYPE_VARIABLE;
            Om_Type_Kind field_kind = OM_TYPE_VARIABLE;
            const bool any_actor_ref =
                ((nominal->system_message_id
                      == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
                  || nominal->system_message_id
                      == OM_SYSTEM_MESSAGE_ID_FILE_READ
                  || nominal->system_message_id
                      == OM_SYSTEM_MESSAGE_ID_FILE_EXISTS
                  || nominal->system_message_id
                      == OM_SYSTEM_MESSAGE_ID_FILE_DELETE
                  || nominal->system_message_id
                      == OM_SYSTEM_MESSAGE_ID_DIRECTORY_CREATE
                  || nominal->system_message_id
                      == OM_SYSTEM_MESSAGE_ID_DIRECTORY_LIST
                  || nominal->system_message_id
                      == OM_SYSTEM_MESSAGE_ID_FILE_STAT)
                 && index == 1)
                || ((nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_CLOCK_MONOTONIC_NOW
                    || nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_CLOCK_UTC_NOW)
                    && index == 0)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_SECURE_RANDOM_BYTES
                    && index == 1)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_LOGGER_WRITE && index == 1)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_LOGGER_FLUSH && index == 0)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_TCP_LISTEN && index == 3)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_TCP_CONNECT && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_TCP_ACCEPT && index == 1)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_TCP_SEND && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_TCP_RECEIVE && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_TCP_CLOSE && index == 1)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_PROCESS_SPAWN && index == 5)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_PROCESS_WRITE_STDIN && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDOUT && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_PROCESS_READ_STDERR && index == 2)
                || ((nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_PROCESS_CLOSE_STDIN
                    || nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_PROCESS_TERMINATE
                    || nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_PROCESS_WAIT) && index == 1)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_UDP_CONNECT && index == 3)
                || ((nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_UDP_SEND
                    || nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_UDP_RECEIVE) && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_UDP_CLOSE && index == 1)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_FILE_WRITE && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_FILE_APPEND && index == 2)
                || (nominal->system_message_id
                        == OM_SYSTEM_MESSAGE_ID_TIMER_AFTER && index == 2);
            const bool any_message = nominal->system_message_id
                    == OM_SYSTEM_MESSAGE_ID_TIMER_AFTER && index == 1;
            if (any_actor_ref || any_message) {
                status = value_type_kind(project, operands[index],
                                         &operand_kind);
                if (status == OM_STATUS_OK) {
                    status = resolve_type(project->type_store, field->type_id,
                                          &object_type_id);
                }
                if (status == OM_STATUS_OK) {
                    Om_Type_View field_type_view = {0};
                    status = om_type_store_get(project->type_store,
                        object_type_id, &field_type_view);
                    if (status == OM_STATUS_OK) {
                        field_kind = field_type_view.kind;
                    }
                }
                const Om_Type_Kind expected_kind = any_actor_ref
                    ? OM_TYPE_ACTOR_REF : OM_TYPE_MESSAGE;
                if (status == OM_STATUS_OK
                    && (operand_kind != expected_kind
                        || field_kind != expected_kind)) {
                    status = add_diagnostic(diagnostics,
                        OM_MIR_DIAGNOSTIC_E1102, function->id, block->id,
                        operands[index], instruction->span);
                }
            } else {
                status = verify_value_type(project, diagnostics, function->id,
                    block->id, operands[index], field->type_id,
                    instruction->span);
            }
            if (status != OM_STATUS_OK) return status;
        }
        break;
    }
    case OM_MIR_LOAD_FIELD: {
        MIR_EXPECT_OPERANDS(1);
        const Om_Mir_Value *object = value_at_const(project, operands[0]);
        Om_Type_Id object_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View object_type = {0};
        status = object == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, object->type_id,
                           &object_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, object_type_id,
                                       &object_type);
        }
        const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
            ? nominal_for_symbol(project, object_type.nominal_symbol,
                                 object_type.kind) : NULL;
        const Om_Mir_Field_Descriptor *field = field_for_symbol(project,
            nominal, instruction->symbol_id);
        if (status != OM_STATUS_OK || nominal == NULL || field == NULL
            || (object_type.kind != OM_TYPE_RECORD
                && object_type.kind != OM_TYPE_MESSAGE)) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        return verify_value_type(project, diagnostics, function->id, block->id,
            result->id, field->type_id, instruction->span);
    }
    case OM_MIR_COPY_MANAGED: {
        MIR_EXPECT_OPERANDS(2);
        Om_Type_Id object_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View object_type = {0};
        status = resolve_type(project->type_store, result->type_id,
                              &object_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, object_type_id,
                                       &object_type);
        }
        const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
            ? nominal_for_symbol(project, object_type.nominal_symbol,
                                 OM_TYPE_RECORD) : NULL;
        const Om_Mir_Field_Descriptor *field = field_for_symbol(project,
            nominal, instruction->symbol_id);
        if (status != OM_STATUS_OK || object_type.kind != OM_TYPE_RECORD
            || nominal == NULL || field == NULL) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        status = verify_value_type(project, diagnostics, function->id,
            block->id, operands[0], result->type_id, instruction->span);
        if (status != OM_STATUS_OK) return status;
        return verify_value_type(project, diagnostics, function->id, block->id,
            operands[1], field->type_id, instruction->span);
    }
    case OM_MIR_INT_ADD_CHECKED:
    case OM_MIR_INT_SUB_CHECKED:
    case OM_MIR_INT_MUL_CHECKED:
    case OM_MIR_INT_DIV_CHECKED:
    case OM_MIR_INT_REM_CHECKED:
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_INT);
        break;
    case OM_MIR_INT_NEGATE_CHECKED:
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_INT);
        break;
    case OM_MIR_FLOAT_ADD:
    case OM_MIR_FLOAT_SUB:
    case OM_MIR_FLOAT_MUL:
    case OM_MIR_FLOAT_DIV:
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_FLOAT);
        break;
    case OM_MIR_FLOAT_NEGATE:
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_FLOAT);
        break;
    case OM_MIR_BOOL_AND:
    case OM_MIR_BOOL_OR:
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_BOOL);
        break;
    case OM_MIR_BOOL_NOT:
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_BOOL);
        break;
    case OM_MIR_COMPARE:
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_BOOL);
        if ((size_t)instruction->operator_kind
                < (size_t)OM_HIR_OPERATOR_EQUAL
            || (size_t)instruction->operator_kind
                > (size_t)OM_HIR_OPERATOR_GREATER_EQUAL) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        {
            const Om_Mir_Value *left = value_at_const(project, operands[0]);
            const Om_Mir_Value *right = value_at_const(project, operands[1]);
            if (left == NULL || right == NULL) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    function->id, block->id, result->id, instruction->span);
            }
            bool same_type = false;
            status = type_equal(project->type_store, left->type_id,
                                right->type_id, &same_type);
            if (status != OM_STATUS_OK || !same_type) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                    function->id, block->id, result->id, instruction->span);
            }
            const bool ordered = instruction->operator_kind
                    != OM_HIR_OPERATOR_EQUAL
                && instruction->operator_kind != OM_HIR_OPERATOR_NOT_EQUAL;
            bool supported = false;
            status = type_supports_compare(project->type_store, left->type_id,
                                           ordered, 0, &supported);
            if (status != OM_STATUS_OK || !supported) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                    function->id, block->id, operands[0], instruction->span);
            }
        }
        break;
    case OM_MIR_CALL_DIRECT: {
        const Om_Mir_Function *callee = function_for_instance(project,
            instruction->callee_instance_id);
        if (callee == NULL) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1105,
                function->id, block->id, result->id, instruction->span);
        }
        if (function->kind == OM_MIR_FUNCTION
            && callee->kind == OM_MIR_ACTION) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                function->id, block->id, result->id, instruction->span);
        }
        if ((callee->effects & ~instruction->effects) != 0) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id callee_type = OM_TYPE_ID_INVALID;
        Om_Type_View callee_view = {0};
        status = resolve_type(project->type_store, callee->signature_type_id,
                              &callee_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, callee_type,
                                       &callee_view);
        }
        if (status != OM_STATUS_OK
            || (callee_view.kind != OM_TYPE_FUNCTION
                && callee_view.kind != OM_TYPE_ACTION)
            || callee_view.child_count == 0
            || operand_count != callee_view.child_count - 1) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        for (size_t index = 0; index < operand_count; ++index) {
            Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
            status = om_type_store_child_at(project->type_store, callee_type,
                index, &parameter_type);
            if (status != OM_STATUS_OK) {
                break;
            }
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[index], parameter_type, instruction->span);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        Om_Type_Id return_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, callee_type,
            callee_view.child_count - 1, &return_type);
        if (status != OM_STATUS_OK) {
            return status;
        }
        return verify_value_type(project, diagnostics, function->id, block->id,
                                 result->id, return_type, instruction->span);
    }
    case OM_MIR_FUNCTION_REF: {
        MIR_EXPECT_OPERANDS(0);
        const Om_Mir_Function *callee = function_for_instance(project,
            instruction->callee_instance_id);
        if (callee == NULL) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1105,
                function->id, block->id, result->id, instruction->span);
        }
        const Om_Mir_Function_Kind expected_kind = result_kind == OM_TYPE_FUNCTION
            ? OM_MIR_FUNCTION : OM_MIR_ACTION;
        if ((result_kind != OM_TYPE_FUNCTION && result_kind != OM_TYPE_ACTION)
            || callee->kind != expected_kind
            || callee->symbol_id.value != instruction->symbol_id.value) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        bool equal = false;
        status = type_equal(project->type_store, result->type_id,
                            callee->signature_type_id, &equal);
        if (status != OM_STATUS_OK || !equal) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    }
    case OM_MIR_CALL_INDIRECT: {
        if (operand_count == 0) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        const Om_Mir_Value *callee_value = value_at_const(project, operands[0]);
        Om_Type_Id callee_type = OM_TYPE_ID_INVALID;
        Om_Type_View callee_view = {0};
        if (callee_value == NULL
            || resolve_type(project->type_store, callee_value->type_id,
                            &callee_type) != OM_STATUS_OK
            || om_type_store_get(project->type_store, callee_type,
                                 &callee_view) != OM_STATUS_OK
            || (callee_view.kind != OM_TYPE_FUNCTION
                && callee_view.kind != OM_TYPE_ACTION)
            || callee_view.child_count == 0
            || operand_count - 1 != callee_view.child_count - 1) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        if (function->kind == OM_MIR_FUNCTION
            && callee_view.kind == OM_TYPE_ACTION) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                function->id, block->id, result->id, instruction->span);
        }
        for (size_t index = 1; index < operand_count; ++index) {
            Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
            status = om_type_store_child_at(project->type_store, callee_type,
                index - 1, &parameter_type);
            if (status != OM_STATUS_OK) {
                break;
            }
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[index], parameter_type, instruction->span);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        Om_Type_Id return_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, callee_type,
            callee_view.child_count - 1, &return_type);
        if (status != OM_STATUS_OK) {
            return status;
        }
        return verify_value_type(project, diagnostics, function->id, block->id,
                                 result->id, return_type, instruction->span);
    }
        break;
    case OM_MIR_STORE_INIT_FIELD: {
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_UNIT);
        const Om_Mir_Value *object = value_at_const(project, operands[0]);
        Om_Type_Id object_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View object_type = {0};
        status = object == NULL ? OM_STATUS_INVALID_ARGUMENT
            : resolve_type(project->type_store, object->type_id,
                           &object_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, object_type_id,
                                       &object_type);
        }
        const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
            ? nominal_for_symbol(project, object_type.nominal_symbol,
                                 object_type.kind) : NULL;
        const Om_Mir_Field_Descriptor *field = field_for_symbol(project,
            nominal, instruction->symbol_id);
        if (status != OM_STATUS_OK || nominal == NULL || field == NULL
            || (object_type.kind != OM_TYPE_RECORD
                && object_type.kind != OM_TYPE_MESSAGE)) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        return verify_value_type(project, diagnostics, function->id, block->id,
            operands[1], field->type_id, instruction->span);
    }
    case OM_MIR_RUNTIME_ACTOR_SPAWN: {
        if (operand_count > 1) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        MIR_EXPECT_RESULT(OM_TYPE_ACTOR_REF);
        if (operand_count == 1) {
            Om_Type_Id node_ref_type = OM_TYPE_ID_INVALID;
            status = om_type_store_primitive(project->type_store,
                OM_TYPE_NODE_REF, &node_ref_type);
            if (status != OM_STATUS_OK) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                    function->id, block->id, operands[0], instruction->span);
            }
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[0], node_ref_type, instruction->span);
            if (status != OM_STATUS_OK) return status;
        }
        bool found_actor = false;
        for (size_t index = 0; index < project->actor_count; ++index) {
            if (project->actors[index].symbol_id.value
                == instruction->symbol_id.value) {
                found_actor = true;
                break;
            }
        }
        if (!found_actor) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_SELF:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_ACTOR_REF);
        break;
    case OM_MIR_RUNTIME_ACTOR_SEND: {
        MIR_EXPECT_OPERANDS(2);
        MIR_EXPECT_RESULT(OM_TYPE_UNIT);
        Om_Type_Kind target_kind = OM_TYPE_VARIABLE;
        Om_Type_Kind message_kind = OM_TYPE_VARIABLE;
        if (value_type_kind(project, operands[0], &target_kind) != OM_STATUS_OK
            || target_kind != OM_TYPE_ACTOR_REF
            || value_type_kind(project, operands[1], &message_kind) != OM_STATUS_OK
            || message_kind != OM_TYPE_MESSAGE) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_WATCH: {
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_UNIT);
        Om_Type_Kind target_kind = OM_TYPE_VARIABLE;
        if (value_type_kind(project, operands[0], &target_kind) != OM_STATUS_OK
            || target_kind != OM_TYPE_ACTOR_REF) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_STOP:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_UNIT);
        if (function->kind != OM_MIR_RECEIVE) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    case OM_MIR_NATIVE_CALL:
    case OM_MIR_NATIVE_CALLBACK_ADDRESS:
    case OM_MIR_CSTRING_CONVERT:
        break;
    case OM_MIR_RUNTIME_SYSTEM_ACTOR:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_ACTOR_REF);
        if (instruction->integer_value != OM_SYSTEM_ACTOR_REF_CONSOLE
            && instruction->integer_value != OM_SYSTEM_ACTOR_REF_TIMER
            && instruction->integer_value
                != OM_SYSTEM_ACTOR_REF_FILE_SYSTEM
            && instruction->integer_value != OM_SYSTEM_ACTOR_REF_CLOCK
            && instruction->integer_value
                != OM_SYSTEM_ACTOR_REF_SECURE_RANDOM
            && instruction->integer_value != OM_SYSTEM_ACTOR_REF_LOGGER
            && instruction->integer_value != OM_SYSTEM_ACTOR_REF_NETWORK
            && instruction->integer_value != OM_SYSTEM_ACTOR_REF_PROCESS) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    case OM_MIR_RUNTIME_FAULT:
        MIR_EXPECT_OPERANDS(0);
        MIR_EXPECT_RESULT(OM_TYPE_UNIT);
        if (instruction->integer_value
                == (uint64_t)OM_MIR_RUNTIME_FAULT_ZERO_RANGE_STEP) {
            break;
        }
        if (instruction->integer_value
                != (uint64_t)OM_MIR_RUNTIME_FAULT_ACTOR_FAILED
            || function->kind != OM_MIR_RECEIVE) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, result->id, instruction->span);
        }
        break;
    default:
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                              function->id, block->id, result->id,
                              instruction->span);
    }
    if (instruction->opcode == OM_MIR_INT_ADD_CHECKED
        || instruction->opcode == OM_MIR_INT_SUB_CHECKED
        || instruction->opcode == OM_MIR_INT_MUL_CHECKED
        || instruction->opcode == OM_MIR_INT_DIV_CHECKED
        || instruction->opcode == OM_MIR_INT_REM_CHECKED
        || instruction->opcode == OM_MIR_INT_NEGATE_CHECKED
        || instruction->opcode == OM_MIR_FLOAT_ADD
        || instruction->opcode == OM_MIR_FLOAT_SUB
        || instruction->opcode == OM_MIR_FLOAT_MUL
        || instruction->opcode == OM_MIR_FLOAT_DIV
        || instruction->opcode == OM_MIR_FLOAT_NEGATE
        || instruction->opcode == OM_MIR_BOOL_AND
        || instruction->opcode == OM_MIR_BOOL_OR
        || instruction->opcode == OM_MIR_BOOL_NOT
        || instruction->opcode == OM_MIR_COMPARE) {
        for (size_t index = 0; index < operand_count; ++index) {
            const Om_Mir_Value *operand = value_at_const(project, operands[index]);
            bool equal = false;
            if (operand == NULL) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    function->id, block->id, operands[index], instruction->span);
            }
            status = type_equal(project->type_store, operand->type_id,
                                value_at_const(project, operands[0])->type_id,
                                &equal);
            if (status != OM_STATUS_OK || !equal) {
                return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                    function->id, block->id, operands[index], instruction->span);
            }
            if (instruction->opcode != OM_MIR_COMPARE) {
                Om_Type_Kind expected_kind = OM_TYPE_INT;
                if (instruction->opcode == OM_MIR_FLOAT_ADD
                    || instruction->opcode == OM_MIR_FLOAT_SUB
                    || instruction->opcode == OM_MIR_FLOAT_MUL
                    || instruction->opcode == OM_MIR_FLOAT_DIV
                    || instruction->opcode == OM_MIR_FLOAT_NEGATE) {
                    expected_kind = OM_TYPE_FLOAT;
                } else if (instruction->opcode == OM_MIR_BOOL_AND
                           || instruction->opcode == OM_MIR_BOOL_OR
                           || instruction->opcode == OM_MIR_BOOL_NOT) {
                    expected_kind = OM_TYPE_BOOL;
                }
                Om_Type_Kind operand_kind = OM_TYPE_VARIABLE;
                status = value_type_kind(project, operands[index],
                                         &operand_kind);
                if (status != OM_STATUS_OK || operand_kind != expected_kind) {
                    return add_diagnostic(diagnostics,
                        OM_MIR_DIAGNOSTIC_E1102, function->id, block->id,
                        operands[index], instruction->span);
                }
            }
        }
    }
    if (instruction->opcode == OM_MIR_NATIVE_CALL
        && function->kind == OM_MIR_RECEIVE
        && (instruction->flags & OM_MIR_INSTRUCTION_FLAG_NATIVE_BLOCKING) != 0) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                              function->id, block->id, result->id,
                              instruction->span);
    }
    const uint32_t known_effects =
        (uint32_t)OM_EFFECT_MUTATES_LOCAL
        | (uint32_t)OM_EFFECT_MUTATES_ACTOR_STATE
        | (uint32_t)OM_EFFECT_SEND_MESSAGE
        | (uint32_t)OM_EFFECT_SPAWN_ACTOR
        | (uint32_t)OM_EFFECT_NATIVE
        | (uint32_t)OM_EFFECT_FILE_IO
        | (uint32_t)OM_EFFECT_NETWORK_IO
        | (uint32_t)OM_EFFECT_CLOCK
        | (uint32_t)OM_EFFECT_WATCH
        | (uint32_t)OM_EFFECT_STOP
        | (uint32_t)OM_EFFECT_CALL_ACTION
        | (uint32_t)OM_EFFECT_READS_ACTOR_STATE;
    if ((instruction->effects & ~known_effects) != 0
        || (instruction->effects & ~function->effects) != 0
        || (instruction->flags
            & ~(uint32_t)OM_MIR_INSTRUCTION_FLAG_NATIVE_BLOCKING) != 0) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                              function->id, block->id, result->id,
                              instruction->span);
    }
    if (function->kind == OM_MIR_FUNCTION
        && (instruction->opcode == OM_MIR_RUNTIME_ACTOR_SPAWN
            || instruction->opcode == OM_MIR_RUNTIME_ACTOR_SELF
            || instruction->opcode == OM_MIR_RUNTIME_ACTOR_SEND
            || instruction->opcode == OM_MIR_RUNTIME_ACTOR_WATCH
            || instruction->opcode == OM_MIR_RUNTIME_ACTOR_STOP
            || instruction->opcode == OM_MIR_RUNTIME_SYSTEM_ACTOR
            || instruction->opcode == OM_MIR_NATIVE_CALLBACK_ADDRESS
            || (instruction->opcode == OM_MIR_NATIVE_CALL
                && (instruction->effects != OM_EFFECT_NONE
                    || (instruction->flags
                        & OM_MIR_INSTRUCTION_FLAG_NATIVE_BLOCKING) != 0)))) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                              function->id, block->id, result->id,
                              instruction->span);
    }
    if (instruction->opcode == OM_MIR_RUNTIME_ACTOR_SELF
        && function->kind != OM_MIR_RECEIVE) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
                              function->id, block->id, result->id,
                              instruction->span);
    }
    if (instruction->opcode == OM_MIR_LIST_GET) {
        MIR_EXPECT_OPERANDS(2);
        Om_Type_Id list_type = OM_TYPE_ID_INVALID;
        Om_Type_View list_view = {0};
        const Om_Mir_Value *list_value = value_at_const(project, operands[0]);
        Om_Type_Kind index_kind = OM_TYPE_VARIABLE;
        if (list_value == NULL
            || resolve_type(project->type_store, list_value->type_id,
                            &list_type) != OM_STATUS_OK
            || om_type_store_get(project->type_store, list_type,
                                 &list_view) != OM_STATUS_OK
            || list_view.kind != OM_TYPE_LIST || list_view.child_count != 1
            || value_type_kind(project, operands[1], &index_kind) != OM_STATUS_OK
            || index_kind != OM_TYPE_INT) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id element_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, list_type, 0,
                                        &element_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, result->id, element_type, instruction->span);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    } else if (instruction->opcode == OM_MIR_MAP_GET) {
        MIR_EXPECT_OPERANDS(2);
        Om_Type_Id map_type = OM_TYPE_ID_INVALID;
        Om_Type_View map_view = {0};
        const Om_Mir_Value *map_value = value_at_const(project, operands[0]);
        if (map_value == NULL
            || resolve_type(project->type_store, map_value->type_id,
                            &map_type) != OM_STATUS_OK
            || om_type_store_get(project->type_store, map_type,
                                 &map_view) != OM_STATUS_OK
            || map_view.kind != OM_TYPE_MAP || map_view.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id key_type = OM_TYPE_ID_INVALID;
        Om_Type_Id value_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, map_type, 0,
                                        &key_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[1], key_type, instruction->span);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(project->type_store, map_type, 1,
                                            &value_type);
        }
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, result->id, value_type, instruction->span);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    } else if (instruction->opcode == OM_MIR_LIST_CREATE) {
        Om_Type_View list_view = {0};
        Om_Type_Id list_type = OM_TYPE_ID_INVALID;
        status = resolve_type(project->type_store, result->type_id, &list_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, list_type,
                                       &list_view);
        }
        if (status != OM_STATUS_OK || list_view.kind != OM_TYPE_LIST
            || list_view.child_count != 1) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id element_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, list_type, 0,
                                        &element_type);
        for (size_t index = 0; status == OM_STATUS_OK
             && index < operand_count; ++index) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[index], element_type, instruction->span);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    } else if (instruction->opcode == OM_MIR_MAP_CREATE) {
        Om_Type_View map_view = {0};
        Om_Type_Id map_type = OM_TYPE_ID_INVALID;
        status = resolve_type(project->type_store, result->type_id, &map_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, map_type, &map_view);
        }
        if (status != OM_STATUS_OK || map_view.kind != OM_TYPE_MAP
            || map_view.child_count != 2 || operand_count % 2 != 0) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id key_type = OM_TYPE_ID_INVALID;
        Om_Type_Id value_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, map_type, 0,
                                        &key_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(project->type_store, map_type, 1,
                                            &value_type);
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < operand_count; index += 2) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[index], key_type, instruction->span);
            if (status == OM_STATUS_OK) {
                status = verify_value_type(project, diagnostics, function->id,
                    block->id, operands[index + 1], value_type, instruction->span);
            }
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    } else if (instruction->opcode == OM_MIR_RESULT_MAKE_OK
               || instruction->opcode == OM_MIR_RESULT_MAKE_ERROR) {
        MIR_EXPECT_OPERANDS(1);
        Om_Type_View result_view = {0};
        Om_Type_Id result_type = OM_TYPE_ID_INVALID;
        status = resolve_type(project->type_store, result->type_id, &result_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(project->type_store, result_type,
                                       &result_view);
        }
        if (status != OM_STATUS_OK || result_view.kind != OM_TYPE_RESULT
            || result_view.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, result->id, instruction->span);
        }
        Om_Type_Id payload_type = OM_TYPE_ID_INVALID;
        const size_t payload_index = instruction->opcode == OM_MIR_RESULT_MAKE_OK
            ? 0 : 1;
        status = om_type_store_child_at(project->type_store, result_type,
                                        payload_index, &payload_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, operands[0], payload_type, instruction->span);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    } else if (instruction->opcode == OM_MIR_RESULT_TAG) {
        MIR_EXPECT_OPERANDS(1);
        MIR_EXPECT_RESULT(OM_TYPE_BOOL);
        Om_Type_Kind input_kind = OM_TYPE_VARIABLE;
        if (value_type_kind(project, operands[0], &input_kind) != OM_STATUS_OK
            || input_kind != OM_TYPE_RESULT) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
    } else if (instruction->opcode == OM_MIR_RESULT_OK_PAYLOAD
               || instruction->opcode == OM_MIR_RESULT_ERROR_PAYLOAD) {
        MIR_EXPECT_OPERANDS(1);
        const Om_Mir_Value *input = value_at_const(project, operands[0]);
        Om_Type_Id input_type = OM_TYPE_ID_INVALID;
        Om_Type_View input_view = {0};
        if (input == NULL
            || resolve_type(project->type_store, input->type_id,
                            &input_type) != OM_STATUS_OK
            || om_type_store_get(project->type_store, input_type,
                                 &input_view) != OM_STATUS_OK
            || input_view.kind != OM_TYPE_RESULT || input_view.child_count != 2) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
                function->id, block->id, operands[0], instruction->span);
        }
        const size_t payload_index =
            instruction->opcode == OM_MIR_RESULT_OK_PAYLOAD ? 0 : 1;
        Om_Type_Id payload_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, input_type,
            payload_index, &payload_type);
        if (status == OM_STATUS_OK) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, result->id, payload_type, instruction->span);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
#undef MIR_EXPECT_RESULT
#undef MIR_EXPECT_OPERANDS
    return OM_STATUS_OK;
}

static Om_Status verify_value_use(const Om_Mir_Project *project,
                                  Om_Vector *diagnostics,
                                  const Om_Mir_Function *function,
                                  const Om_Mir_Block *use_block,
                                  size_t use_local_index,
                                  size_t use_instruction_index,
                                  Om_Mir_Value_Id value_id,
                                  const uint32_t *local_index,
                                  const bool *dominators,
                                  size_t function_block_count,
                                  Om_Source_Span span)
{
    const Om_Mir_Value *value = value_at_const(project, value_id);
    if (value == NULL || value->defining_block_id.value >= project->block_count) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1103,
                              function->id, use_block->id, value_id, span);
    }
    if (value->defining_block_id.value == use_block->id.value) {
        if (!value->is_block_parameter
            && value->defining_instruction_id.value >= use_instruction_index) {
            return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1103,
                                  function->id, use_block->id, value_id, span);
        }
        return OM_STATUS_OK;
    }
    const size_t def_local_index = local_index[value->defining_block_id.value];
    if (def_local_index >= function_block_count
        || !dominators[use_local_index * function_block_count + def_local_index]) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1103,
                              function->id, use_block->id, value_id, span);
    }
    return OM_STATUS_OK;
}

static Om_Status verify_function_dominance(const Om_Mir_Project *project,
                                           Om_Vector *diagnostics,
                                           const Om_Mir_Function *function)
{
    size_t function_block_count = 0;
    for (size_t index = 0; index < project->block_count; ++index) {
        if (project->blocks[index].function_id.value == function->id.value) {
            ++function_block_count;
        }
    }
    if (function_block_count == 0
        || function_block_count > OM_MIR_MAX_BLOCKS_PER_FUNCTION
        || function_block_count > SIZE_MAX / function_block_count) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                              function->id, OM_MIR_BLOCK_ID_INVALID,
                              OM_MIR_VALUE_ID_INVALID, function->span);
    }
    Om_Mir_Block_Id *function_blocks = om_alloc_array(function_block_count,
                                                       sizeof(*function_blocks));
    uint32_t *local_index = om_alloc_array(project->block_count,
                                           sizeof(*local_index));
    bool *reachable = om_alloc_array(function_block_count, sizeof(*reachable));
    bool *dominators = om_alloc_array(function_block_count * function_block_count,
                                      sizeof(*dominators));
    size_t *queue = om_alloc_array(function_block_count, sizeof(*queue));
    bool *next_row = om_alloc_array(function_block_count, sizeof(*next_row));
    if (function_blocks == NULL || local_index == NULL || reachable == NULL
        || dominators == NULL || queue == NULL || next_row == NULL) {
        om_free(next_row);
        om_free(queue);
        om_free(dominators);
        om_free(reachable);
        om_free(local_index);
        om_free(function_blocks);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < project->block_count; ++index) {
        local_index[index] = UINT32_MAX;
    }
    size_t next_function_block = 0;
    for (size_t index = 0; index < project->block_count; ++index) {
        if (project->blocks[index].function_id.value == function->id.value) {
            function_blocks[next_function_block] = project->blocks[index].id;
            local_index[project->blocks[index].id.value] =
                (uint32_t)next_function_block;
            ++next_function_block;
        }
    }
    const size_t entry_local_index =
        function->entry_block_id.value < project->block_count
            ? local_index[function->entry_block_id.value] : SIZE_MAX;
    if (entry_local_index >= function_block_count) {
        om_free(next_row);
        om_free(queue);
        om_free(dominators);
        om_free(reachable);
        om_free(local_index);
        om_free(function_blocks);
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
            function->id, function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
            function->span);
    }
    (void)memset(reachable, 0, function_block_count * sizeof(*reachable));
    (void)memset(dominators, 0,
        function_block_count * function_block_count * sizeof(*dominators));
    size_t queue_read = 0;
    size_t queue_write = 0;
    reachable[entry_local_index] = true;
    queue[queue_write++] = entry_local_index;
    while (queue_read < queue_write) {
        const size_t source_local = queue[queue_read++];
        const Om_Mir_Block *source = block_at_const(project,
            function_blocks[source_local]);
        if (source == NULL || !source->has_terminator) {
            continue;
        }
        for (size_t edge_index = 0; edge_index < source->terminator.edge_count;
             ++edge_index) {
            const Om_Mir_Edge *edge = &project->edges[
                source->terminator.edge_start + edge_index];
            if (edge->target_block_id.value >= project->block_count) {
                continue;
            }
            const size_t target_local = local_index[edge->target_block_id.value];
            if (target_local < function_block_count && !reachable[target_local]) {
                reachable[target_local] = true;
                queue[queue_write++] = target_local;
            }
        }
    }
    for (size_t block_index = 0; block_index < function_block_count;
         ++block_index) {
        bool *row = &dominators[block_index * function_block_count];
        if (block_index == entry_local_index || !reachable[block_index]) {
            row[block_index] = true;
        } else {
            for (size_t candidate = 0; candidate < function_block_count;
                 ++candidate) {
                row[candidate] = reachable[candidate];
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t block_index = 0; block_index < function_block_count;
             ++block_index) {
            if (block_index == entry_local_index || !reachable[block_index]) {
                continue;
            }
            (void)memset(next_row, 0, function_block_count * sizeof(*next_row));
            bool has_predecessor = false;
            const Om_Mir_Block_Id target_id = function_blocks[block_index];
            for (size_t predecessor_index = 0;
                 predecessor_index < function_block_count; ++predecessor_index) {
                if (!reachable[predecessor_index]) {
                    continue;
                }
                const Om_Mir_Block *predecessor = block_at_const(project,
                    function_blocks[predecessor_index]);
                if (predecessor == NULL || !predecessor->has_terminator) {
                    continue;
                }
                for (size_t edge_index = 0;
                     edge_index < predecessor->terminator.edge_count;
                     ++edge_index) {
                    const Om_Mir_Edge *edge = &project->edges[
                        predecessor->terminator.edge_start + edge_index];
                    if (edge->target_block_id.value == target_id.value) {
                        if (!has_predecessor) {
                            (void)memcpy(next_row,
                                &dominators[predecessor_index
                                    * function_block_count],
                                function_block_count * sizeof(*next_row));
                            has_predecessor = true;
                        } else {
                            for (size_t candidate = 0;
                                 candidate < function_block_count; ++candidate) {
                                next_row[candidate] = next_row[candidate]
                                    && dominators[predecessor_index
                                        * function_block_count + candidate];
                            }
                        }
                    }
                }
            }
            if (!has_predecessor) {
                (void)memset(next_row, 0,
                    function_block_count * sizeof(*next_row));
            }
            next_row[block_index] = true;
            bool *row = &dominators[block_index * function_block_count];
            if (memcmp(row, next_row,
                       function_block_count * sizeof(*row)) != 0) {
                (void)memcpy(row, next_row,
                    function_block_count * sizeof(*row));
                changed = true;
            }
        }
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t block_index = 0; status == OM_STATUS_OK
         && block_index < function_block_count; ++block_index) {
        const Om_Mir_Block *block = block_at_const(project,
            function_blocks[block_index]);
        if (block == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        for (size_t instruction_offset = 0;
             status == OM_STATUS_OK
             && instruction_offset < block->instruction_count;
             ++instruction_offset) {
            const Om_Mir_Instruction *instruction = &project->instructions[
                block->instruction_start + instruction_offset];
            for (size_t operand_offset = 0;
                 status == OM_STATUS_OK
                 && operand_offset < instruction->operand_count;
                 ++operand_offset) {
                const Om_Mir_Value_Id value_id = project->operands[
                    instruction->operand_start + operand_offset];
                status = verify_value_use(project, diagnostics, function, block,
                    block_index, instruction->id.value, value_id, local_index,
                    dominators, function_block_count, instruction->span);
            }
        }
        if (status != OM_STATUS_OK || !block->has_terminator) {
            continue;
        }
        if (block->terminator.value_id.value != UINT32_MAX) {
            status = verify_value_use(project, diagnostics, function, block,
                block_index, project->instruction_count,
                block->terminator.value_id, local_index, dominators,
                function_block_count, function->span);
        }
        for (size_t edge_offset = 0; status == OM_STATUS_OK
             && edge_offset < block->terminator.edge_count; ++edge_offset) {
            const Om_Mir_Edge *edge = &project->edges[
                block->terminator.edge_start + edge_offset];
            for (size_t argument_offset = 0; status == OM_STATUS_OK
                 && argument_offset < edge->argument_count; ++argument_offset) {
                const Om_Mir_Value_Id value_id = project->edge_arguments[
                    edge->argument_start + argument_offset];
                status = verify_value_use(project, diagnostics, function, block,
                    block_index, project->instruction_count, value_id,
                    local_index, dominators, function_block_count, function->span);
            }
        }
    }
    om_free(next_row);
    om_free(queue);
    om_free(dominators);
    om_free(reachable);
    om_free(local_index);
    om_free(function_blocks);
    return status;
}

static Om_Status verify_function(const Om_Mir_Project *project,
                                 Om_Vector *diagnostics,
                                 const Om_Mir_Function *function)
{
    if (function->instance_id.value == UINT32_MAX) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1105,
            function->id, function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
            function->span);
    }
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Type_View signature_view = {0};
    Om_Status status = resolve_type(project->type_store,
        function->signature_type_id, &signature);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(project->type_store, signature,
                                   &signature_view);
    }
    bool concrete_signature = false;
    if (status == OM_STATUS_OK) {
        status = type_is_concrete(project->type_store, signature, 0,
                                  &concrete_signature);
    }
    if (status != OM_STATUS_OK || !concrete_signature
        || (signature_view.kind != OM_TYPE_FUNCTION
            && signature_view.kind != OM_TYPE_ACTION)
        || signature_view.child_count == 0) {
        return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1105,
            function->id, function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
            function->span);
    }
    if (function->kind == OM_MIR_FUNCTION
        && signature_view.kind != OM_TYPE_FUNCTION) {
        status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
            function->id, function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
            function->span);
    } else if (function->kind == OM_MIR_ACTION
               && signature_view.kind != OM_TYPE_ACTION) {
        status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1102,
            function->id, function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
            function->span);
    }
    const uint32_t forbidden_function_effects =
        (uint32_t)OM_EFFECT_MUTATES_ACTOR_STATE
        | (uint32_t)OM_EFFECT_SEND_MESSAGE
        | (uint32_t)OM_EFFECT_SPAWN_ACTOR
        | (uint32_t)OM_EFFECT_NATIVE
        | (uint32_t)OM_EFFECT_FILE_IO
        | (uint32_t)OM_EFFECT_NETWORK_IO
        | (uint32_t)OM_EFFECT_CLOCK
        | (uint32_t)OM_EFFECT_WATCH
        | (uint32_t)OM_EFFECT_STOP
        | (uint32_t)OM_EFFECT_CALL_ACTION
        | (uint32_t)OM_EFFECT_READS_ACTOR_STATE;
    const uint32_t known_function_effects = forbidden_function_effects
        | (uint32_t)OM_EFFECT_MUTATES_LOCAL;
    if ((function->effects & ~known_function_effects) != 0) {
        status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
            function->id, function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
            function->span);
    }
    if (function->kind == OM_MIR_FUNCTION
        && (function->effects & forbidden_function_effects) != 0) {
        status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1104,
            function->id, function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
            function->span);
    }
    const Om_Mir_Block *entry = block_at_const(project,
        function->entry_block_id);
    if (entry == NULL || entry->function_id.value != function->id.value
        || entry->parameter_count + 1 != signature_view.child_count) {
        const Om_Status added = add_diagnostic(diagnostics,
            OM_MIR_DIAGNOSTIC_E1101, function->id,
            function->entry_block_id, OM_MIR_VALUE_ID_INVALID, function->span);
        return status == OM_STATUS_OK ? added : status;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < entry->parameter_count; ++index) {
        Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(project->type_store, signature,
                                        index, &parameter_type);
        if (status == OM_STATUS_OK) {
            const Om_Mir_Value_Id value_id = {
                entry->parameter_start + (uint32_t)index,
            };
            status = verify_value_type(project, diagnostics, function->id,
                entry->id, value_id, parameter_type, function->span);
        }
    }
    for (size_t block_index = 0; status == OM_STATUS_OK
         && block_index < project->block_count; ++block_index) {
        const Om_Mir_Block *block = &project->blocks[block_index];
        if (block->function_id.value != function->id.value) {
            continue;
        }
        for (size_t parameter_index = 0;
             status == OM_STATUS_OK
             && parameter_index < block->parameter_count; ++parameter_index) {
            const Om_Mir_Value_Id parameter_id = {
                block->parameter_start + (uint32_t)parameter_index,
            };
            const Om_Mir_Value *parameter = value_at_const(project, parameter_id);
            bool concrete = false;
            if (parameter == NULL
                || type_is_concrete(project->type_store, parameter->type_id, 0,
                                    &concrete) != OM_STATUS_OK
                || !concrete) {
                status = add_diagnostic(diagnostics,
                    OM_MIR_DIAGNOSTIC_E1105, function->id, block->id,
                    parameter_id, function->span);
            }
        }
        if (status != OM_STATUS_OK) {
            break;
        }
        if (!block->has_terminator) {
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, OM_MIR_VALUE_ID_INVALID, function->span);
            continue;
        }
        const Om_Mir_Terminator *terminator = &block->terminator;
        if (terminator->edge_start > project->edge_count
            || terminator->edge_count
                > project->edge_count - terminator->edge_start) {
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, terminator->value_id, function->span);
            continue;
        }
        if ((terminator->kind == OM_MIR_BR && terminator->edge_count != 1)
            || (terminator->kind == OM_MIR_COND_BR
                && terminator->edge_count != 2)
            || (terminator->kind == OM_MIR_SWITCH
                && terminator->edge_count == 0)
            || ((terminator->kind == OM_MIR_RETURN
                 || terminator->kind == OM_MIR_UNREACHABLE)
                && terminator->edge_count != 0)) {
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, terminator->value_id, function->span);
            continue;
        }
        if ((terminator->kind == OM_MIR_COND_BR
             || terminator->kind == OM_MIR_SWITCH)
            && terminator->value_id.value == UINT32_MAX) {
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, terminator->value_id, function->span);
            continue;
        }
        if ((terminator->kind == OM_MIR_BR
             || terminator->kind == OM_MIR_UNREACHABLE)
            && terminator->value_id.value != UINT32_MAX) {
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, terminator->value_id, function->span);
            continue;
        }
        if (terminator->kind == OM_MIR_COND_BR) {
            status = verify_value_type(project, diagnostics, function->id,
                block->id, terminator->value_id,
                project->type_store->primitive_types[OM_TYPE_BOOL], function->span);
        } else if (terminator->kind == OM_MIR_SWITCH) {
            Om_Type_Kind switch_kind = OM_TYPE_VARIABLE;
            if (value_type_kind(project, terminator->value_id, &switch_kind)
                    != OM_STATUS_OK
                || switch_kind != OM_TYPE_INT) {
                status = add_diagnostic(diagnostics,
                    OM_MIR_DIAGNOSTIC_E1102, function->id, block->id,
                    terminator->value_id, function->span);
            }
            size_t default_count = 0;
            for (size_t edge_offset = 0; status == OM_STATUS_OK
                 && edge_offset < terminator->edge_count; ++edge_offset) {
                const Om_Mir_Edge *edge = &project->edges[
                    terminator->edge_start + edge_offset];
                if (edge->is_default_case) {
                    ++default_count;
                    if (edge->has_case_value) {
                        status = add_diagnostic(diagnostics,
                            OM_MIR_DIAGNOSTIC_E1101, function->id, block->id,
                            terminator->value_id, function->span);
                    }
                } else if (!edge->has_case_value) {
                    status = add_diagnostic(diagnostics,
                        OM_MIR_DIAGNOSTIC_E1101, function->id, block->id,
                        terminator->value_id, function->span);
                    break;
                }
                for (size_t other = edge_offset + 1;
                     other < terminator->edge_count; ++other) {
                    const Om_Mir_Edge *other_edge = &project->edges[
                        terminator->edge_start + other];
                    if (edge->has_case_value && other_edge->has_case_value
                        && edge->case_value == other_edge->case_value) {
                        status = add_diagnostic(diagnostics,
                            OM_MIR_DIAGNOSTIC_E1101, function->id, block->id,
                            terminator->value_id, function->span);
                        break;
                    }
                }
                if (status != OM_STATUS_OK) {
                    break;
                }
            }
            if (status == OM_STATUS_OK && default_count != 1) {
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    function->id, block->id, terminator->value_id, function->span);
            }
        }
        if (status != OM_STATUS_OK) {
            break;
        }
        if (terminator->kind == OM_MIR_RETURN) {
            Om_Type_Id return_type = OM_TYPE_ID_INVALID;
            status = om_type_store_child_at(project->type_store, signature,
                signature_view.child_count - 1, &return_type);
            if (status == OM_STATUS_OK
                && terminator->value_id.value != UINT32_MAX) {
                status = verify_value_type(project, diagnostics, function->id,
                    block->id, terminator->value_id, return_type, function->span);
            } else if (status == OM_STATUS_OK) {
                Om_Type_Id resolved_return = OM_TYPE_ID_INVALID;
                Om_Type_View return_view = {0};
                status = resolve_type(project->type_store, return_type,
                                      &resolved_return);
                if (status == OM_STATUS_OK) {
                    status = om_type_store_get(project->type_store,
                        resolved_return, &return_view);
                }
                if (status == OM_STATUS_OK && return_view.kind != OM_TYPE_UNIT) {
                    status = add_diagnostic(diagnostics,
                        OM_MIR_DIAGNOSTIC_E1102, function->id, block->id,
                        OM_MIR_VALUE_ID_INVALID, function->span);
                }
            }
        } else if (terminator->kind == OM_MIR_UNREACHABLE
                   && terminator->value_id.value != UINT32_MAX) {
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                function->id, block->id, terminator->value_id, function->span);
        }
        for (size_t edge_offset = 0; status == OM_STATUS_OK
             && edge_offset < terminator->edge_count; ++edge_offset) {
            const Om_Mir_Edge *edge = &project->edges[
                terminator->edge_start + edge_offset];
            if (terminator->kind != OM_MIR_SWITCH
                && (edge->has_case_value || edge->is_default_case)) {
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    function->id, block->id, terminator->value_id, function->span);
                break;
            }
            const Om_Mir_Block *target = block_at_const(project,
                edge->target_block_id);
            if (target == NULL || target->function_id.value != function->id.value
                || edge->argument_start > project->edge_argument_count
                || edge->argument_count
                    > project->edge_argument_count - edge->argument_start
                || edge->argument_count != target->parameter_count) {
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    function->id, block->id, terminator->value_id, function->span);
                break;
            }
            for (size_t argument_index = 0;
                 status == OM_STATUS_OK
                 && argument_index < edge->argument_count; ++argument_index) {
                const Om_Mir_Value_Id argument_id = project->edge_arguments[
                    edge->argument_start + argument_index];
                const Om_Mir_Value_Id parameter_id = {
                    target->parameter_start + (uint32_t)argument_index,
                };
                const Om_Mir_Value *parameter = value_at_const(project,
                    parameter_id);
                if (parameter == NULL) {
                    status = add_diagnostic(diagnostics,
                        OM_MIR_DIAGNOSTIC_E1101, function->id, block->id,
                        argument_id, function->span);
                } else {
                    status = verify_value_type(project, diagnostics,
                        function->id, block->id, argument_id,
                        parameter->type_id, function->span);
                }
            }
        }
        for (size_t instruction_offset = 0; status == OM_STATUS_OK
             && instruction_offset < block->instruction_count;
             ++instruction_offset) {
            const Om_Mir_Instruction *instruction = &project->instructions[
                block->instruction_start + instruction_offset];
            status = verify_instruction(project, diagnostics, function, block,
                                        instruction);
        }
    }
    if (status == OM_STATUS_OK) {
        status = verify_function_dominance(project, diagnostics, function);
    }
    return status;
}

static Om_Status validate_structure(const Om_Mir_Project *project,
                                    Om_Vector *diagnostics,
                                    bool *structurally_valid);

static Om_Status validate_nominal_descriptors(
    const Om_Mir_Project *project,
    Om_Vector *diagnostics,
    bool *structurally_valid)
{
    *structurally_valid = true;
    const Om_Mir_Function_Id invalid_function = OM_MIR_FUNCTION_ID_INVALID;
    const Om_Mir_Block_Id invalid_block = OM_MIR_BLOCK_ID_INVALID;
    const Om_Mir_Value_Id invalid_value = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->nominal_count; ++index) {
        const Om_Mir_Nominal_Descriptor *nominal = &project->nominals[index];
        if (nominal->symbol_id.value == UINT32_MAX
            || (nominal->type_kind != OM_TYPE_RECORD
                && nominal->type_kind != OM_TYPE_MESSAGE)
            || !range_is_valid(nominal->field_start, nominal->field_count,
                               project->field_count)) {
            *structurally_valid = false;
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                invalid_function, invalid_block, invalid_value,
                (Om_Source_Span){0});
            continue;
        }
        for (size_t other = index + 1; other < project->nominal_count; ++other) {
            if (nominal->symbol_id.value
                == project->nominals[other].symbol_id.value) {
                *structurally_valid = false;
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    invalid_function, invalid_block, invalid_value,
                    (Om_Source_Span){0});
                break;
            }
        }
        for (size_t field_index = 0;
             status == OM_STATUS_OK && field_index < nominal->field_count;
             ++field_index) {
            const Om_Mir_Field_Descriptor *field = &project->fields[
                nominal->field_start + field_index];
            bool concrete = false;
            if (field->symbol_id.value == UINT32_MAX
                || type_is_concrete(project->type_store, field->type_id, 0,
                                    &concrete) != OM_STATUS_OK) {
                *structurally_valid = false;
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    invalid_function, invalid_block, invalid_value,
                    (Om_Source_Span){0});
            } else if (!concrete) {
                *structurally_valid = false;
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1105,
                    invalid_function, invalid_block, invalid_value,
                    (Om_Source_Span){0});
            }
            for (size_t other_nominal = 0;
                 status == OM_STATUS_OK
                 && other_nominal <= index; ++other_nominal) {
                const Om_Mir_Nominal_Descriptor *other =
                    &project->nominals[other_nominal];
                if (!range_is_valid(other->field_start, other->field_count,
                                    project->field_count)) {
                    continue;
                }
                const size_t other_limit = other_nominal == index
                    ? field_index : other->field_count;
                for (size_t other_index = 0; other_index < other_limit;
                     ++other_index) {
                    const Om_Mir_Field_Descriptor *previous =
                        &project->fields[other->field_start + other_index];
                    if (previous->symbol_id.value == field->symbol_id.value) {
                        *structurally_valid = false;
                        status = add_diagnostic(diagnostics,
                            OM_MIR_DIAGNOSTIC_E1101, invalid_function,
                            invalid_block, invalid_value,
                            (Om_Source_Span){0});
                        break;
                    }
                }
            }
        }
    }
    for (size_t field_index = 0; status == OM_STATUS_OK
         && field_index < project->field_count; ++field_index) {
        size_t owners = 0;
        for (size_t nominal_index = 0;
             nominal_index < project->nominal_count; ++nominal_index) {
            const Om_Mir_Nominal_Descriptor *nominal =
                &project->nominals[nominal_index];
            if (range_is_valid(nominal->field_start, nominal->field_count,
                               project->field_count)
                && field_index >= nominal->field_start
                && field_index - nominal->field_start < nominal->field_count) {
                ++owners;
            }
        }
        if (owners != 1) {
            *structurally_valid = false;
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                invalid_function, invalid_block, invalid_value,
                (Om_Source_Span){0});
        }
    }
    return status;
}

static Om_Status validate_actor_descriptors(
    const Om_Mir_Project *project,
    Om_Vector *diagnostics,
    bool *structurally_valid)
{
    *structurally_valid = true;
    const Om_Mir_Function_Id invalid_function = OM_MIR_FUNCTION_ID_INVALID;
    const Om_Mir_Block_Id invalid_block = OM_MIR_BLOCK_ID_INVALID;
    const Om_Mir_Value_Id invalid_value = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->actor_count; ++index) {
        const Om_Mir_Actor_Descriptor *actor = &project->actors[index];
        Om_Type_Id state_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View state_type = {0};
        const bool ranges_valid = range_is_valid(
                actor->state_initializer_start,
                actor->state_initializer_count,
                project->actor_state_initializer_count)
            && range_is_valid(actor->handler_start, actor->handler_count,
                              project->actor_handler_count);
        if (actor->symbol_id.value == UINT32_MAX || !ranges_valid
            || resolve_type(project->type_store, actor->state_type_id,
                            &state_type_id) != OM_STATUS_OK
            || om_type_store_get(project->type_store, state_type_id,
                                 &state_type) != OM_STATUS_OK
            || state_type.kind != OM_TYPE_RECORD
            || state_type.nominal_symbol.value != actor->symbol_id.value
            || nominal_for_symbol(project, actor->symbol_id, OM_TYPE_RECORD)
                == NULL) {
            *structurally_valid = false;
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                invalid_function, invalid_block, invalid_value,
                (Om_Source_Span){0});
            continue;
        }
        for (size_t other = index + 1; other < project->actor_count; ++other) {
            if (project->actors[other].symbol_id.value
                == actor->symbol_id.value) {
                *structurally_valid = false;
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    invalid_function, invalid_block, invalid_value,
                    (Om_Source_Span){0});
                break;
            }
        }
        const Om_Mir_Nominal_Descriptor *state_nominal = nominal_for_symbol(
            project, actor->symbol_id, OM_TYPE_RECORD);
        if (state_nominal == NULL
            || actor->state_initializer_count != state_nominal->field_count) {
            *structurally_valid = false;
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                invalid_function, invalid_block, invalid_value,
                (Om_Source_Span){0});
        }
        for (size_t field_index = 0; status == OM_STATUS_OK
             && field_index < actor->state_initializer_count; ++field_index) {
            const Om_Mir_Actor_State_Initializer *initializer =
                &project->actor_state_initializers[
                    actor->state_initializer_start + field_index];
            const Om_Mir_Function *function = function_for_instance(project,
                initializer->function_instance_id);
            bool field_found = false;
            for (size_t state_field = 0; state_nominal != NULL
                 && state_field < state_nominal->field_count; ++state_field) {
                const Om_Mir_Field_Descriptor *field = &project->fields[
                    state_nominal->field_start + state_field];
                if (field->symbol_id.value
                    == initializer->field_symbol_id.value) {
                    field_found = true;
                    break;
                }
            }
            if (!field_found || function == NULL
                || function->kind != OM_MIR_ACTOR_STATE_INIT) {
                *structurally_valid = false;
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    invalid_function, invalid_block, invalid_value,
                    function != NULL ? function->span : (Om_Source_Span){0});
            }
            for (size_t other_actor = 0; other_actor <= index; ++other_actor) {
                const Om_Mir_Actor_Descriptor *owner =
                    &project->actors[other_actor];
                if (!range_is_valid(owner->state_initializer_start,
                        owner->state_initializer_count,
                        project->actor_state_initializer_count)) {
                    continue;
                }
                const size_t limit = other_actor == index
                    ? field_index : owner->state_initializer_count;
                for (size_t other = 0; other < limit; ++other) {
                    const Om_Mir_Actor_State_Initializer *prior =
                        &project->actor_state_initializers[
                            owner->state_initializer_start + other];
                    if (prior->function_instance_id.value
                        == initializer->function_instance_id.value
                        || prior->field_symbol_id.value
                            == initializer->field_symbol_id.value) {
                        *structurally_valid = false;
                        status = add_diagnostic(diagnostics,
                            OM_MIR_DIAGNOSTIC_E1101, invalid_function,
                            invalid_block, invalid_value,
                            (Om_Source_Span){0});
                        break;
                    }
                }
            }
        }
        for (size_t handler_index = 0; status == OM_STATUS_OK
             && handler_index < actor->handler_count; ++handler_index) {
            const Om_Mir_Actor_Handler *handler = &project->actor_handlers[
                actor->handler_start + handler_index];
            const Om_Mir_Function *function = function_for_instance(project,
                handler->function_instance_id);
            Om_Type_Id message_type_id = OM_TYPE_ID_INVALID;
            Om_Type_View message_type = {0};
            if (nominal_for_symbol(project, handler->message_symbol_id,
                                   OM_TYPE_MESSAGE) == NULL
                || resolve_type(project->type_store, handler->message_type_id,
                                &message_type_id) != OM_STATUS_OK
                || om_type_store_get(project->type_store, message_type_id,
                                     &message_type) != OM_STATUS_OK
                || message_type.kind != OM_TYPE_MESSAGE
                || message_type.nominal_symbol.value
                    != handler->message_symbol_id.value
                || function == NULL || function->kind != OM_MIR_RECEIVE) {
                *structurally_valid = false;
                status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                    invalid_function, invalid_block, invalid_value,
                    function != NULL ? function->span : (Om_Source_Span){0});
            }
            for (size_t other_actor = 0; other_actor <= index; ++other_actor) {
                const Om_Mir_Actor_Descriptor *owner =
                    &project->actors[other_actor];
                if (!range_is_valid(owner->handler_start, owner->handler_count,
                                    project->actor_handler_count)) {
                    continue;
                }
                const size_t limit = other_actor == index
                    ? handler_index : owner->handler_count;
                for (size_t other = 0; other < limit; ++other) {
                    const Om_Mir_Actor_Handler *prior =
                        &project->actor_handlers[owner->handler_start + other];
                    if ((other_actor == index
                         && prior->message_symbol_id.value
                            == handler->message_symbol_id.value)
                        || prior->function_instance_id.value
                            == handler->function_instance_id.value) {
                        *structurally_valid = false;
                        status = add_diagnostic(diagnostics,
                            OM_MIR_DIAGNOSTIC_E1101, invalid_function,
                            invalid_block, invalid_value,
                            (Om_Source_Span){0});
                        break;
                    }
                }
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->actor_state_initializer_count; ++index) {
        size_t owners = 0;
        for (size_t actor_index = 0; actor_index < project->actor_count;
             ++actor_index) {
            const Om_Mir_Actor_Descriptor *actor =
                &project->actors[actor_index];
            if (range_is_valid(actor->state_initializer_start,
                    actor->state_initializer_count,
                    project->actor_state_initializer_count)
                && index >= actor->state_initializer_start
                && index - actor->state_initializer_start
                    < actor->state_initializer_count) {
                ++owners;
            }
        }
        if (owners != 1) {
            *structurally_valid = false;
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                invalid_function, invalid_block, invalid_value,
                (Om_Source_Span){0});
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->actor_handler_count; ++index) {
        size_t owners = 0;
        for (size_t actor_index = 0; actor_index < project->actor_count;
             ++actor_index) {
            const Om_Mir_Actor_Descriptor *actor =
                &project->actors[actor_index];
            if (range_is_valid(actor->handler_start, actor->handler_count,
                    project->actor_handler_count)
                && index >= actor->handler_start
                && index - actor->handler_start < actor->handler_count) {
                ++owners;
            }
        }
        if (owners != 1) {
            *structurally_valid = false;
            status = add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                invalid_function, invalid_block, invalid_value,
                (Om_Source_Span){0});
        }
    }
    return status;
}

Om_Status om_mir_verify_project(const Om_Mir_Project *project,
                                Om_Mir_Verification_Result *result)
{
    if (project == NULL || result == NULL || project->owned_storage == NULL
        || project->type_store == NULL || result->owned_diagnostics != NULL
        || (project->function_count != 0 && project->functions == NULL)
        || (project->block_count != 0 && project->blocks == NULL)
        || (project->value_count != 0 && project->values == NULL)
        || (project->instruction_count != 0 && project->instructions == NULL)
        || (project->operand_count != 0 && project->operands == NULL)
        || (project->edge_count != 0 && project->edges == NULL)
        || (project->edge_argument_count != 0
            && project->edge_arguments == NULL)
        || (project->nominal_count != 0 && project->nominals == NULL)
        || (project->field_count != 0 && project->fields == NULL)
        || (project->actor_count != 0 && project->actors == NULL)
        || (project->actor_state_initializer_count != 0
            && project->actor_state_initializers == NULL)
        || (project->actor_handler_count != 0
            && project->actor_handlers == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Vector diagnostics = {0};
    Om_Status status = om_vector_init(&diagnostics, sizeof(Om_Mir_Diagnostic));
    bool structurally_valid = true;
    if (status == OM_STATUS_OK) {
        status = validate_nominal_descriptors(project, &diagnostics,
                                               &structurally_valid);
    }
    if (status == OM_STATUS_OK && structurally_valid) {
        status = validate_actor_descriptors(project, &diagnostics,
                                             &structurally_valid);
    }
    if (status == OM_STATUS_OK && structurally_valid
        && project->actor_down_message_symbol_id.value != UINT32_MAX) {
        Om_Type_Id down_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View down_type = {0};
        const Om_Mir_Nominal_Descriptor *down_nominal = nominal_for_symbol(
            project, project->actor_down_message_symbol_id, OM_TYPE_MESSAGE);
        if (down_nominal == NULL
            || down_nominal->field_count != 2
            || resolve_type(project->type_store,
                project->actor_down_message_type_id, &down_type_id) != OM_STATUS_OK
            || om_type_store_get(project->type_store, down_type_id,
                                 &down_type) != OM_STATUS_OK
            || down_type.kind != OM_TYPE_MESSAGE
            || down_type.nominal_symbol.value
                != project->actor_down_message_symbol_id.value) {
            structurally_valid = false;
            status = add_diagnostic(&diagnostics, OM_MIR_DIAGNOSTIC_E1101,
                OM_MIR_FUNCTION_ID_INVALID, OM_MIR_BLOCK_ID_INVALID,
                OM_MIR_VALUE_ID_INVALID, (Om_Source_Span){0});
        }
    } else if (status == OM_STATUS_OK && structurally_valid
        && project->actor_down_message_type_id.value != UINT32_MAX) {
        structurally_valid = false;
        status = add_diagnostic(&diagnostics, OM_MIR_DIAGNOSTIC_E1101,
            OM_MIR_FUNCTION_ID_INVALID, OM_MIR_BLOCK_ID_INVALID,
            OM_MIR_VALUE_ID_INVALID, (Om_Source_Span){0});
    }
    if (status == OM_STATUS_OK && structurally_valid) {
        status = validate_structure(project, &diagnostics, &structurally_valid);
    }
    for (size_t index = 0; status == OM_STATUS_OK && structurally_valid
         && index < project->function_count; ++index) {
        const Om_Mir_Function *function = &project->functions[index];
        status = verify_function(project, &diagnostics, function);
    }
    if (status != OM_STATUS_OK) {
        om_vector_destroy(&diagnostics);
        return status;
    }
    result->diagnostics = diagnostics.data;
    result->diagnostic_count = diagnostics.length;
    result->owned_diagnostics = diagnostics.data;
    diagnostics.data = NULL;
    om_vector_destroy(&diagnostics);
    return OM_STATUS_OK;
}

void om_mir_verification_result_destroy(Om_Mir_Verification_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->owned_diagnostics);
    *result = (Om_Mir_Verification_Result){0};
}

static bool range_is_valid(size_t start, size_t count, size_t total)
{
    return start <= total && count <= total - start;
}

static Om_Status structural_diagnostic(Om_Vector *diagnostics,
                                       Om_Mir_Function_Id function_id,
                                       Om_Mir_Block_Id block_id,
                                       Om_Mir_Value_Id value_id,
                                       Om_Source_Span span,
                                       bool *structurally_valid)
{
    *structurally_valid = false;
    return add_diagnostic(diagnostics, OM_MIR_DIAGNOSTIC_E1101, function_id,
                          block_id, value_id, span);
}

static Om_Status validate_structure(const Om_Mir_Project *project,
                                    Om_Vector *diagnostics,
                                    bool *structurally_valid)
{
    bool *seen_values = project->value_count == 0 ? NULL
        : om_alloc_array(project->value_count, sizeof(*seen_values));
    bool *seen_instructions = project->instruction_count == 0 ? NULL
        : om_alloc_array(project->instruction_count, sizeof(*seen_instructions));
    bool *seen_edges = project->edge_count == 0 ? NULL
        : om_alloc_array(project->edge_count, sizeof(*seen_edges));
    if ((project->value_count != 0 && seen_values == NULL)
        || (project->instruction_count != 0 && seen_instructions == NULL)
        || (project->edge_count != 0 && seen_edges == NULL)) {
        om_free(seen_edges);
        om_free(seen_instructions);
        om_free(seen_values);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (seen_values != NULL) {
        (void)memset(seen_values, 0,
                     project->value_count * sizeof(*seen_values));
    }
    if (seen_instructions != NULL) {
        (void)memset(seen_instructions, 0,
                     project->instruction_count * sizeof(*seen_instructions));
    }
    if (seen_edges != NULL) {
        (void)memset(seen_edges, 0, project->edge_count * sizeof(*seen_edges));
    }
    *structurally_valid = true;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->function_count; ++index) {
        const Om_Mir_Function *function = &project->functions[index];
        if (function->id.value != index
            || (size_t)function->kind > (size_t)OM_MIR_ACTOR_STATE_INIT) {
            status = structural_diagnostic(diagnostics, function->id,
                function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
                function->span, structurally_valid);
        }
        if (status == OM_STATUS_OK
            && function->instance_id.value != UINT32_MAX) {
            for (size_t other = index + 1; other < project->function_count;
                 ++other) {
                if (project->functions[other].instance_id.value
                    == function->instance_id.value) {
                    status = structural_diagnostic(diagnostics, function->id,
                        function->entry_block_id, OM_MIR_VALUE_ID_INVALID,
                        function->span, structurally_valid);
                    break;
                }
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->block_count; ++index) {
        const Om_Mir_Block *block = &project->blocks[index];
        if (block->id.value != index
            || block->function_id.value >= project->function_count
            || !range_is_valid(block->parameter_start, block->parameter_count,
                               project->value_count)
            || !range_is_valid(block->instruction_start,
                               block->instruction_count,
                               project->instruction_count)) {
            status = structural_diagnostic(diagnostics, block->function_id,
                block->id, OM_MIR_VALUE_ID_INVALID, (Om_Source_Span){0},
                structurally_valid);
            continue;
        }
        if (!block->has_terminator
            || block->terminator.kind == OM_MIR_TERMINATOR_NONE
            || (size_t)block->terminator.kind > (size_t)OM_MIR_UNREACHABLE
            || !range_is_valid(block->terminator.edge_start,
                               block->terminator.edge_count,
                               project->edge_count)) {
            status = structural_diagnostic(diagnostics, block->function_id,
                block->id, block->terminator.value_id, (Om_Source_Span){0},
                structurally_valid);
            if (status != OM_STATUS_OK) {
                break;
            }
        } else {
            for (size_t edge_index = 0;
                 status == OM_STATUS_OK
                 && edge_index < block->terminator.edge_count; ++edge_index) {
                const size_t global_edge_index =
                    block->terminator.edge_start + edge_index;
                if (seen_edges[global_edge_index]) {
                    status = structural_diagnostic(diagnostics,
                        block->function_id, block->id,
                        block->terminator.value_id, (Om_Source_Span){0},
                        structurally_valid);
                } else {
                    seen_edges[global_edge_index] = true;
                }
            }
        }
        for (size_t parameter_index = 0;
             status == OM_STATUS_OK
             && parameter_index < block->parameter_count; ++parameter_index) {
            const size_t value_index = block->parameter_start + parameter_index;
            const Om_Mir_Value *value = &project->values[value_index];
            if (seen_values[value_index]
                || value->id.value != value_index
                || !value->is_block_parameter
                || value->defining_block_id.value != block->id.value
                || value->defining_instruction_id.value != UINT32_MAX) {
                status = structural_diagnostic(diagnostics, block->function_id,
                    block->id, value->id, (Om_Source_Span){0},
                    structurally_valid);
            } else {
                seen_values[value_index] = true;
            }
        }
        for (size_t instruction_offset = 0;
             status == OM_STATUS_OK
             && instruction_offset < block->instruction_count;
             ++instruction_offset) {
            const size_t instruction_index =
                block->instruction_start + instruction_offset;
            const Om_Mir_Instruction *instruction =
                &project->instructions[instruction_index];
            if (seen_instructions[instruction_index]
                || instruction->id.value != instruction_index
                || instruction->block_id.value != block->id.value
                || !range_is_valid(instruction->operand_start,
                    instruction->operand_count, project->operand_count)
                || instruction->result_id.value >= project->value_count) {
                status = structural_diagnostic(diagnostics, block->function_id,
                    block->id, instruction->result_id, instruction->span,
                    structurally_valid);
                continue;
            }
            const size_t result_index = instruction->result_id.value;
            const Om_Mir_Value *value = &project->values[result_index];
            if (seen_values[result_index]
                || value->id.value != result_index
                || value->is_block_parameter
                || value->defining_block_id.value != block->id.value
                || value->defining_instruction_id.value != instruction_index) {
                status = structural_diagnostic(diagnostics, block->function_id,
                    block->id, instruction->result_id, instruction->span,
                    structurally_valid);
                continue;
            }
            seen_values[result_index] = true;
            seen_instructions[instruction_index] = true;
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->value_count; ++index) {
        const Om_Mir_Value *value = &project->values[index];
        if (value->id.value != index
            || value->defining_block_id.value >= project->block_count
            || !seen_values[index]) {
            status = structural_diagnostic(diagnostics,
                OM_MIR_FUNCTION_ID_INVALID, value->defining_block_id,
                value->id, (Om_Source_Span){0}, structurally_valid);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->instruction_count; ++index) {
        const Om_Mir_Instruction *instruction = &project->instructions[index];
        if (!seen_instructions[index]
            || instruction->block_id.value >= project->block_count) {
            status = structural_diagnostic(diagnostics,
                OM_MIR_FUNCTION_ID_INVALID, instruction->block_id,
                instruction->result_id, instruction->span,
                structurally_valid);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->edge_count; ++index) {
        const Om_Mir_Edge *edge = &project->edges[index];
        if (!seen_edges[index]
            || edge->target_block_id.value >= project->block_count
            || !range_is_valid(edge->argument_start, edge->argument_count,
                               project->edge_argument_count)) {
            status = structural_diagnostic(diagnostics,
                OM_MIR_FUNCTION_ID_INVALID, edge->target_block_id,
                OM_MIR_VALUE_ID_INVALID, (Om_Source_Span){0},
                structurally_valid);
        }
    }
    om_free(seen_edges);
    om_free(seen_instructions);
    om_free(seen_values);
    return status;
}

typedef struct Om_Mir_Text_Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Om_Mir_Text_Buffer;

static Om_Status text_append(Om_Mir_Text_Buffer *buffer,
                             const char *format,
                             ...)
{
    char temporary[512];
    va_list args;
    va_start(args, format);
    const int required_int = vsnprintf(temporary, sizeof(temporary), format, args);
    va_end(args);
    if (required_int < 0 || (size_t)required_int >= sizeof(temporary)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t required = (size_t)required_int;
    if (required > SIZE_MAX - buffer->length - 1) {
        return OM_STATUS_OVERFLOW;
    }
    const size_t with_terminator = buffer->length + required + 1;
    if (with_terminator > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 512 : buffer->capacity;
        while (capacity < with_terminator) {
            if (capacity > SIZE_MAX / 2) {
                capacity = with_terminator;
                break;
            }
            capacity *= 2;
        }
        char *data = om_realloc_array(buffer->data, capacity, sizeof(*data));
        if (data == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    (void)memcpy(buffer->data + buffer->length, temporary, required);
    buffer->length += required;
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

const char *om_mir_opcode_name(Om_Mir_Opcode opcode)
{
    static const char *const names[] = {
        "const-int", "const-float", "const-bool", "const-null",
        "int-add-checked",
        "int-sub-checked", "int-mul-checked", "int-div-checked",
        "int-rem-checked", "int-negate-checked", "float-add", "float-sub",
        "float-mul", "float-div", "float-negate", "bool-and", "bool-or",
        "bool-not", "compare", "call-direct", "function-ref",
        "call-indirect", "alloc-object",
        "load-field", "store-init-field", "copy-managed", "list-create",
        "list-concat", "list-length", "list-get", "map-create", "map-length",
        "map-key-at", "map-get", "result-make-ok",
        "result-make-error", "result-tag", "result-ok-payload",
        "result-error-payload", "runtime-string-create", "runtime-string-format",
        "runtime-string-concat", "runtime-actor-spawn", "runtime-actor-self",
        "runtime-actor-send",
        "runtime-actor-watch", "runtime-actor-stop",
        "runtime-system-actor", "runtime-fault", "native-call",
        "native-callback-address", "cstring-convert", "runtime-print",
    };
    return (size_t)opcode < sizeof(names) / sizeof(names[0])
        ? names[opcode] : "invalid-mir-opcode";
}

const char *om_mir_terminator_name(Om_Mir_Terminator_Kind kind)
{
    static const char *const names[] = {
        "none", "br", "cond-br", "switch", "return", "unreachable",
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid-mir-terminator";
}

const char *om_mir_diagnostic_code_name(Om_Mir_Diagnostic_Code code)
{
    switch (code) {
    case OM_MIR_DIAGNOSTIC_E1101: return "E1101";
    case OM_MIR_DIAGNOSTIC_E1102: return "E1102";
    case OM_MIR_DIAGNOSTIC_E1103: return "E1103";
    case OM_MIR_DIAGNOSTIC_E1104: return "E1104";
    case OM_MIR_DIAGNOSTIC_E1105: return "E1105";
    case OM_MIR_DIAGNOSTIC_E1106: return "E1106";
    default: return "E0000";
    }
}

static Om_Status append_type(Om_Mir_Text_Buffer *buffer,
                             const Om_Type_Store *store,
                             Om_Type_Id type_id)
{
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(store, type_id, &resolved);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved, &view);
    }
    if (status == OM_STATUS_OK) {
        status = text_append(buffer, "t%" PRIu32 "(%s)", resolved.value,
                             om_type_kind_name(view.kind));
    }
    return status;
}

Om_Status om_mir_dump(const Om_Mir_Project *project, Om_Mir_Text *result)
{
    if (project == NULL || result == NULL || project->owned_storage == NULL
        || result->data != NULL || result->length != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Text_Buffer buffer = {0};
    Om_Status status = OM_STATUS_OK;
    for (size_t function_index = 0; status == OM_STATUS_OK
         && function_index < project->function_count; ++function_index) {
        const Om_Mir_Function *function = &project->functions[function_index];
        status = text_append(&buffer,
            "function f%" PRIu32 " instance=%" PRIu32 " kind=%u"
            " symbol=%" PRIu32 " signature=",
            function->id.value, function->instance_id.value,
            (unsigned)function->kind, function->symbol_id.value);
        if (status == OM_STATUS_OK) {
            status = append_type(&buffer, project->type_store,
                                 function->signature_type_id);
        }
        if (status == OM_STATUS_OK) {
            status = text_append(&buffer, " entry=bb%" PRIu32 " effects=%" PRIu32
                "\n", function->entry_block_id.value, function->effects);
        }
        for (size_t block_index = 0; status == OM_STATUS_OK
             && block_index < project->block_count; ++block_index) {
            const Om_Mir_Block *block = &project->blocks[block_index];
            if (block->function_id.value != function->id.value) {
                continue;
            }
            status = text_append(&buffer, "bb%" PRIu32 "(", block->id.value);
            for (size_t parameter_index = 0; status == OM_STATUS_OK
                 && parameter_index < block->parameter_count; ++parameter_index) {
                const Om_Mir_Value *parameter = &project->values[
                    block->parameter_start + parameter_index];
                status = text_append(&buffer, "%sv%" PRIu32 ":",
                    parameter_index == 0 ? "" : ", ", parameter->id.value);
                if (status == OM_STATUS_OK) {
                    status = append_type(&buffer, project->type_store,
                                         parameter->type_id);
                }
            }
            if (status == OM_STATUS_OK) {
                status = text_append(&buffer, "):\n");
            }
            for (size_t instruction_offset = 0; status == OM_STATUS_OK
                 && instruction_offset < block->instruction_count;
                 ++instruction_offset) {
                const Om_Mir_Instruction *instruction = &project->instructions[
                    block->instruction_start + instruction_offset];
                const Om_Mir_Value *value = &project->values[
                    instruction->result_id.value];
                status = text_append(&buffer, "  v%" PRIu32 " = %s",
                    instruction->result_id.value,
                    om_mir_opcode_name(instruction->opcode));
                for (size_t operand_index = 0; status == OM_STATUS_OK
                     && operand_index < instruction->operand_count;
                     ++operand_index) {
                    const Om_Mir_Value_Id operand = project->operands[
                        instruction->operand_start + operand_index];
                    status = text_append(&buffer, "%sv%" PRIu32,
                        operand_index == 0 ? " " : ", ", operand.value);
                }
                if (status == OM_STATUS_OK
                    && instruction->opcode == OM_MIR_CONST_INT) {
                    status = text_append(&buffer, " value=%" PRIu64,
                                         instruction->integer_value);
                }
                if (status == OM_STATUS_OK
                    && instruction->opcode == OM_MIR_CONST_FLOAT) {
                    status = text_append(&buffer, " value=%.17g",
                                         instruction->float_value);
                }
                if (status == OM_STATUS_OK
                    && instruction->opcode == OM_MIR_CONST_BOOL) {
                    status = text_append(&buffer, " value=%s",
                        instruction->integer_value != 0 ? "true" : "false");
                }
                if (status == OM_STATUS_OK
                    && instruction->opcode == OM_MIR_RUNTIME_STRING_CREATE) {
                    status = text_append(&buffer, " value=\"");
                    for (size_t string_index = 0;
                         status == OM_STATUS_OK
                             && string_index < instruction->string_length;
                         ++string_index) {
                        const unsigned char byte = (unsigned char)
                            project->string_data[instruction->string_start
                                + string_index];
                        switch (byte) {
                        case '\\': status = text_append(&buffer, "\\\\"); break;
                        case '"': status = text_append(&buffer, "\\\""); break;
                        case '\n': status = text_append(&buffer, "\\n"); break;
                        case '\r': status = text_append(&buffer, "\\r"); break;
                        case '\t': status = text_append(&buffer, "\\t"); break;
                        case '\0': status = text_append(&buffer, "\\0"); break;
                        default:
                            if (byte < 0x20 || byte == 0x7f) {
                                status = text_append(&buffer, "\\x%02X",
                                                     (unsigned)byte);
                            } else {
                                status = text_append(&buffer, "%c", (int)byte);
                            }
                            break;
                        }
                    }
                    if (status == OM_STATUS_OK) {
                        status = text_append(&buffer, "\"");
                    }
                }
                if (status == OM_STATUS_OK) {
                    status = text_append(&buffer, " : ");
                }
                if (status == OM_STATUS_OK) {
                    status = append_type(&buffer, project->type_store,
                                         value->type_id);
                }
                if (status == OM_STATUS_OK) {
                    status = text_append(&buffer, "\n");
                }
            }
            if (status == OM_STATUS_OK) {
                status = text_append(&buffer, "  %s",
                    om_mir_terminator_name(block->terminator.kind));
            }
            if (status == OM_STATUS_OK
                && block->terminator.value_id.value != UINT32_MAX) {
                status = text_append(&buffer, " v%" PRIu32,
                    block->terminator.value_id.value);
            }
            for (size_t edge_offset = 0; status == OM_STATUS_OK
                 && edge_offset < block->terminator.edge_count; ++edge_offset) {
                const Om_Mir_Edge *edge = &project->edges[
                    block->terminator.edge_start + edge_offset];
                status = text_append(&buffer, "%sbb%" PRIu32,
                    edge_offset == 0 ? " " : ", ",
                    edge->target_block_id.value);
                if (status == OM_STATUS_OK && edge->has_case_value) {
                    status = text_append(&buffer, " case=%" PRId64,
                                         edge->case_value);
                }
                if (status == OM_STATUS_OK && edge->is_default_case) {
                    status = text_append(&buffer, " default");
                }
                if (status == OM_STATUS_OK) {
                    status = text_append(&buffer, "(");
                }
                for (size_t argument_index = 0; status == OM_STATUS_OK
                     && argument_index < edge->argument_count;
                     ++argument_index) {
                    const Om_Mir_Value_Id argument = project->edge_arguments[
                        edge->argument_start + argument_index];
                    status = text_append(&buffer, "%sv%" PRIu32,
                        argument_index == 0 ? "" : ", ", argument.value);
                }
                if (status == OM_STATUS_OK) {
                    status = text_append(&buffer, ")");
                }
            }
            if (status == OM_STATUS_OK) {
                status = text_append(&buffer, "\n");
            }
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(buffer.data);
        return status;
    }
    result->data = buffer.data;
    result->length = buffer.length;
    return OM_STATUS_OK;
}

void om_mir_text_destroy(Om_Mir_Text *text)
{
    if (text == NULL) {
        return;
    }
    om_free(text->data);
    *text = (Om_Mir_Text){0};
}
