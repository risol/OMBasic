#include "ombasic/bytecode/bytecode.h"

#include <string.h>

#include "bytecode_internal.h"
#include "ombasic/foundation/fingerprint.h"
#include "ombasic/foundation/memory.h"

Om_Status om_bytecode_storage_create(Om_Bytecode_Storage **storage)
{
    if (storage == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *storage = NULL;
    Om_Bytecode_Storage *created = om_alloc(sizeof(*created));
    if (created == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *created = (Om_Bytecode_Storage){.active_function = UINT32_MAX};
    Om_Status status = om_vector_init(&created->constants,
                                      sizeof(Om_Bytecode_Constant));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&created->functions,
                                sizeof(Om_Bytecode_Function));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&created->instructions,
                                sizeof(Om_Bytecode_Instruction));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&created->source_map,
                                sizeof(Om_Bytecode_Source_Map_Entry));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&created->actors, sizeof(Om_Bytecode_Actor));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&created->handlers,
                                sizeof(Om_Bytecode_Actor_Handler));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&created->schemas, sizeof(Om_Bytecode_Schema));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&created->schema_fields,
                                sizeof(Om_Bytecode_Schema_Field));
    }
    if (status != OM_STATUS_OK) {
        om_bytecode_storage_destroy(created);
        return status;
    }
    *storage = created;
    return OM_STATUS_OK;
}

static void destroy_constants(Om_Vector *constants)
{
    for (size_t index = 0; index < constants->length; ++index) {
        Om_Bytecode_Constant *constant = om_vector_at(constants, index);
        if (constant != NULL && constant->kind == OM_BYTECODE_CONSTANT_STRING) {
            om_free((void *)constant->as.string_value.data);
        }
    }
}

void om_bytecode_storage_destroy(Om_Bytecode_Storage *storage)
{
    if (storage == NULL) {
        return;
    }
    destroy_constants(&storage->constants);
    om_vector_destroy(&storage->constants);
    om_vector_destroy(&storage->functions);
    om_vector_destroy(&storage->instructions);
    om_vector_destroy(&storage->source_map);
    om_vector_destroy(&storage->actors);
    om_vector_destroy(&storage->handlers);
    om_vector_destroy(&storage->schemas);
    om_vector_destroy(&storage->schema_fields);
    om_free(storage);
}

void om_bytecode_storage_bind_image(
    const Om_Bytecode_Storage *storage,
    Om_Bytecode_Image *image
)
{
    image->constants = storage->constants.data;
    image->constant_count = storage->constants.length;
    image->functions = storage->functions.data;
    image->function_count = storage->functions.length;
    image->instructions = storage->instructions.data;
    image->instruction_count = storage->instructions.length;
    image->source_map = storage->source_map.data;
    image->source_map_count = storage->source_map.length;
    image->actors = storage->actors.data;
    image->actor_count = storage->actors.length;
    image->handlers = storage->handlers.data;
    image->handler_count = storage->handlers.length;
    image->schemas = storage->schemas.data;
    image->schema_count = storage->schemas.length;
    image->schema_fields = storage->schema_fields.data;
    image->schema_field_count = storage->schema_fields.length;
}

static Om_Status clone_constant(const Om_Bytecode_Constant *source,
                                Om_Bytecode_Constant *target)
{
    *target = *source;
    if (source->kind != OM_BYTECODE_CONSTANT_STRING) {
        return OM_STATUS_OK;
    }
    if (source->as.string_value.length > 0
        && source->as.string_value.data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    char *copy = NULL;
    if (source->as.string_value.length > 0) {
        copy = om_alloc(source->as.string_value.length);
        if (copy == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memcpy(copy, source->as.string_value.data,
                     source->as.string_value.length);
    }
    target->as.string_value.data = copy;
    return OM_STATUS_OK;
}

Om_Status om_bytecode_storage_clone_image(
    const Om_Bytecode_Image *image,
    Om_Bytecode_Storage **storage
)
{
    if (image == NULL || storage == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *storage = NULL;
    Om_Bytecode_Storage *created = NULL;
    Om_Status status = om_bytecode_storage_create(&created);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Vector *destinations[] = {
        &created->constants, &created->functions, &created->instructions,
        &created->source_map, &created->actors, &created->handlers,
        &created->schemas, &created->schema_fields
    };
    const void *sources[] = {
        image->constants, image->functions, image->instructions,
        image->source_map, image->actors, image->handlers,
        image->schemas, image->schema_fields
    };
    const size_t counts[] = {
        image->constant_count, image->function_count, image->instruction_count,
        image->source_map_count, image->actor_count, image->handler_count,
        image->schema_count, image->schema_field_count
    };
    for (size_t group = 0; group < 8 && status == OM_STATUS_OK; ++group) {
        if (counts[group] > 0 && sources[group] == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        for (size_t index = 0; index < counts[group]; ++index) {
            const void *item = (const unsigned char *)sources[group]
                + index * destinations[group]->item_size;
            if (group == 0) {
                Om_Bytecode_Constant copy = {0};
                status = clone_constant(item, &copy);
                if (status == OM_STATUS_OK) {
                    status = om_vector_push((Om_Vector *)destinations[group],
                                            &copy);
                }
                if (status != OM_STATUS_OK && copy.kind
                    == OM_BYTECODE_CONSTANT_STRING) {
                    om_free((void *)copy.as.string_value.data);
                }
            } else {
                status = om_vector_push((Om_Vector *)destinations[group], item);
            }
            if (status != OM_STATUS_OK) {
                break;
            }
        }
    }
    if (status != OM_STATUS_OK) {
        om_bytecode_storage_destroy(created);
        return status;
    }
    *storage = created;
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_init(Om_Bytecode_Builder *builder)
{
    if (builder == NULL || builder->owned_storage != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bytecode_Storage *storage = NULL;
    const Om_Status status = om_bytecode_storage_create(&storage);
    if (status == OM_STATUS_OK) {
        builder->owned_storage = storage;
    }
    return status;
}

void om_bytecode_builder_destroy(Om_Bytecode_Builder *builder)
{
    if (builder == NULL) {
        return;
    }
    om_bytecode_storage_destroy(builder->owned_storage);
    builder->owned_storage = NULL;
}

static Om_Bytecode_Storage *builder_storage(Om_Bytecode_Builder *builder)
{
    return builder != NULL ? builder->owned_storage : NULL;
}

Om_Status om_bytecode_builder_add_constant(
    Om_Bytecode_Builder *builder,
    const Om_Bytecode_Constant *constant,
    uint32_t *constant_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    if (storage == NULL || constant == NULL || constant_index == NULL
        || storage->constants.length >= UINT32_MAX
        || constant->kind > OM_BYTECODE_CONSTANT_STRING) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bytecode_Constant owned = {0};
    const Om_Status clone_status = clone_constant(constant, &owned);
    if (clone_status != OM_STATUS_OK) {
        return clone_status;
    }
    const Om_Status status = om_vector_push(&storage->constants, &owned);
    if (status != OM_STATUS_OK) {
        if (owned.kind == OM_BYTECODE_CONSTANT_STRING) {
            om_free((void *)owned.as.string_value.data);
        }
        return status;
    }
    *constant_index = (uint32_t)(storage->constants.length - 1);
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_begin_function(
    Om_Bytecode_Builder *builder,
    uint32_t name_constant,
    uint32_t parameter_count,
    uint32_t flags,
    uint32_t *function_index
)
{
    Om_Status status = om_bytecode_builder_reserve_function(
        builder, name_constant, parameter_count, flags, function_index);
    if (status != OM_STATUS_OK) {
        return status;
    }
    return om_bytecode_builder_begin_function_body(builder, *function_index);
}

Om_Status om_bytecode_builder_reserve_function(
    Om_Bytecode_Builder *builder,
    uint32_t name_constant,
    uint32_t parameter_count,
    uint32_t flags,
    uint32_t *function_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    if (storage == NULL || function_index == NULL
        || storage->active_function != UINT32_MAX
        || storage->functions.length >= UINT32_MAX
        || (flags & OM_BYTECODE_FUNCTION_EXTERNAL) != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bytecode_Function function = {
        .name_constant = name_constant,
        .entry_pc = UINT32_MAX,
        .end_pc = UINT32_MAX,
        .parameter_count = parameter_count,
        .local_count = parameter_count,
        .max_stack = 0,
        .flags = flags
    };
    const Om_Status status = om_vector_push(&storage->functions, &function);
    if (status != OM_STATUS_OK) {
        return status;
    }
    *function_index = (uint32_t)(storage->functions.length - 1);
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_begin_function_body(
    Om_Bytecode_Builder *builder,
    uint32_t function_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    Om_Bytecode_Function *function = storage != NULL
        ? om_vector_at(&storage->functions, function_index) : NULL;
    if (function == NULL || storage->active_function != UINT32_MAX
        || (function->flags & OM_BYTECODE_FUNCTION_EXTERNAL) != 0
        || function->entry_pc != UINT32_MAX
        || storage->instructions.length > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    function->entry_pc = (uint32_t)storage->instructions.length;
    function->end_pc = function->entry_pc;
    storage->active_function = function_index;
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_add_external_function(
    Om_Bytecode_Builder *builder,
    uint32_t name_constant,
    uint32_t parameter_count,
    uint32_t flags,
    uint32_t *function_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    if (storage == NULL || function_index == NULL
        || storage->active_function != UINT32_MAX
        || storage->functions.length >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bytecode_Function function = {
        .name_constant = name_constant,
        .entry_pc = 0,
        .end_pc = 0,
        .parameter_count = parameter_count,
        .local_count = parameter_count,
        .max_stack = 0,
        .flags = flags | OM_BYTECODE_FUNCTION_EXTERNAL
    };
    const Om_Status status = om_vector_push(&storage->functions, &function);
    if (status == OM_STATUS_OK) {
        *function_index = (uint32_t)(storage->functions.length - 1);
    }
    return status;
}

Om_Status om_bytecode_builder_set_function_layout(
    Om_Bytecode_Builder *builder,
    uint32_t function_index,
    uint32_t parameter_count,
    uint32_t local_count
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    Om_Bytecode_Function *function = storage != NULL
        ? om_vector_at(&storage->functions, function_index) : NULL;
    if (function == NULL || function_index != storage->active_function
        || parameter_count != function->parameter_count
        || local_count < parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    function->local_count = local_count;
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_end_function(
    Om_Bytecode_Builder *builder,
    uint32_t function_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    Om_Bytecode_Function *function = storage != NULL
        ? om_vector_at(&storage->functions, function_index) : NULL;
    if (function == NULL || storage->active_function != function_index
        || storage->instructions.length > UINT32_MAX
        || storage->instructions.length == function->entry_pc) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    function->end_pc = (uint32_t)storage->instructions.length;
    storage->active_function = UINT32_MAX;
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_emit(
    Om_Bytecode_Builder *builder,
    Om_Bytecode_Opcode opcode,
    uint32_t operand_a,
    uint32_t operand_b,
    int64_t immediate,
    Om_Source_Span span,
    uint32_t *instruction_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    if (storage == NULL || storage->active_function == UINT32_MAX
        || opcode < OM_BC_NOP || opcode >= OM_BC_OPCODE_COUNT
        || storage->instructions.length >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t pc = (uint32_t)storage->instructions.length;
    const Om_Bytecode_Instruction instruction = {
        .opcode = opcode,
        .operand_a = operand_a,
        .operand_b = operand_b,
        .immediate = immediate
    };
    Om_Status status = om_vector_push(&storage->instructions, &instruction);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (span.source.value != UINT32_MAX && span.start <= span.end) {
        Om_Bytecode_Source_Map_Entry *previous = storage->source_map.length > 0
            ? om_vector_at(&storage->source_map, storage->source_map.length - 1)
            : NULL;
        if (previous != NULL && previous->end_pc == pc
            && previous->span.source.value == span.source.value
            && previous->span.start == span.start
            && previous->span.end == span.end) {
            previous->end_pc = pc + 1;
        } else {
            const Om_Bytecode_Source_Map_Entry entry = {
                .start_pc = pc, .end_pc = pc + 1, .span = span
            };
            status = om_vector_push(&storage->source_map, &entry);
        }
    }
    if (status != OM_STATUS_OK) {
        --storage->instructions.length;
        return status;
    }
    if (instruction_index != NULL) {
        *instruction_index = pc;
    }
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_patch_jump(
    Om_Bytecode_Builder *builder,
    uint32_t instruction_index,
    uint32_t target_pc
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    Om_Bytecode_Instruction *instruction = storage != NULL
        ? om_vector_at(&storage->instructions, instruction_index) : NULL;
    if (instruction == NULL
        || (instruction->opcode != OM_BC_JUMP
            && instruction->opcode != OM_BC_JUMP_IF_FALSE
            && instruction->opcode != OM_BC_JUMP_IF_TRUE)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    instruction->operand_a = target_pc;
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_add_actor(
    Om_Bytecode_Builder *builder,
    Om_Symbol_Id actor_symbol,
    uint32_t initializer_function,
    uint32_t state_count,
    uint32_t *actor_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    if (storage == NULL || actor_index == NULL
        || storage->active_function != UINT32_MAX
        || actor_symbol.value == UINT32_MAX
        || storage->actors.length >= UINT32_MAX
        || storage->handlers.length > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Bytecode_Actor actor = {
        .actor_symbol = actor_symbol,
        .initializer_function = initializer_function,
        .state_count = state_count,
        .first_handler = (uint32_t)storage->handlers.length,
        .handler_count = 0
    };
    const Om_Status status = om_vector_push(&storage->actors, &actor);
    if (status == OM_STATUS_OK) {
        *actor_index = (uint32_t)(storage->actors.length - 1);
    }
    return status;
}

Om_Status om_bytecode_builder_add_actor_handler(
    Om_Bytecode_Builder *builder,
    uint32_t actor_index,
    Om_Symbol_Id message_symbol,
    uint32_t function_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    Om_Bytecode_Actor *actor = storage != NULL
        ? om_vector_at(&storage->actors, actor_index) : NULL;
    if (actor == NULL || storage->active_function != UINT32_MAX
        || actor_index + 1 != storage->actors.length
        || message_symbol.value == UINT32_MAX
        || storage->handlers.length >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Bytecode_Actor_Handler handler = {
        .message_symbol = message_symbol,
        .function_index = function_index
    };
    const Om_Status status = om_vector_push(&storage->handlers, &handler);
    if (status == OM_STATUS_OK) {
        ++actor->handler_count;
    }
    return status;
}

Om_Status om_bytecode_builder_add_schema(
    Om_Bytecode_Builder *builder,
    Om_Symbol_Id symbol_id,
    Om_Bytecode_Schema_Kind kind,
    const uint32_t *field_name_constants,
    size_t field_count,
    uint64_t fingerprint,
    uint32_t *schema_index
)
{
    Om_Bytecode_Storage *storage = builder_storage(builder);
    if (storage == NULL || schema_index == NULL
        || storage->active_function != UINT32_MAX
        || symbol_id.value == UINT32_MAX
        || kind > OM_BYTECODE_SCHEMA_MESSAGE
        || field_count > UINT32_MAX
        || (field_count > 0 && field_name_constants == NULL)
        || storage->schemas.length >= UINT32_MAX
        || storage->schema_fields.length > UINT32_MAX
        || field_count > UINT32_MAX - storage->schema_fields.length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t first_field = storage->schema_fields.length;
    for (size_t index = 0; index < field_count; ++index) {
        const Om_Bytecode_Schema_Field field = {
            .name_constant = field_name_constants[index]
        };
        const Om_Status status = om_vector_push(&storage->schema_fields, &field);
        if (status != OM_STATUS_OK) {
            storage->schema_fields.length = first_field;
            return status;
        }
    }
    const Om_Bytecode_Schema schema = {
        .symbol_id = symbol_id,
        .kind = kind,
        .first_field = (uint32_t)first_field,
        .field_count = (uint32_t)field_count,
        .fingerprint = fingerprint
    };
    const Om_Status status = om_vector_push(&storage->schemas, &schema);
    if (status != OM_STATUS_OK) {
        storage->schema_fields.length = first_field;
        return status;
    }
    *schema_index = (uint32_t)(storage->schemas.length - 1);
    return OM_STATUS_OK;
}

static Om_Status fingerprint_system_schema(
    const char *schema_name,
    const char *const *field_type_names,
    size_t field_count,
    uint64_t *fingerprint)
{
    static const char module_path[] = "__ombasic__";
    static const char type_domain[] = "OMBASIC-TYPE-V1";
    static const char schema_domain[] = "OMBASIC-SCHEMA-V1";
    if (schema_name == NULL || fingerprint == NULL
        || (field_count != 0 && field_type_names == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint64_t field_fingerprints[3] = {0};
    if (field_count > sizeof(field_fingerprints)
                            / sizeof(field_fingerprints[0])) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    for (size_t index = 0; index < field_count; ++index) {
        const char *type_name = field_type_names[index];
        if (type_name == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Status status = om_fingerprint64_schema(
            (Om_String_View){type_domain, sizeof(type_domain) - 1},
            (Om_String_View){NULL, 0},
            (Om_String_View){type_name, strlen(type_name)}, NULL, 0,
            &field_fingerprints[index]);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return om_fingerprint64_schema(
        (Om_String_View){schema_domain, sizeof(schema_domain) - 1},
        (Om_String_View){module_path, sizeof(module_path) - 1},
        (Om_String_View){schema_name, strlen(schema_name)}, field_fingerprints,
        field_count, fingerprint);
}

Om_Status om_bytecode_builder_add_system_actor_schemas(
    Om_Bytecode_Builder *builder)
{
    static const struct {
        uint32_t symbol_id;
        const char *schema_name;
        const char *field_names[3];
        const char *field_types[3];
        size_t field_count;
    } definitions[] = {
        {OM_SYSTEM_MESSAGE_CONSOLE_WRITE, "Console_Write",
         {"text", "reply_to"}, {"String", "ActorRef"}, 2},
        {OM_SYSTEM_MESSAGE_CONSOLE_WRITE_COMPLETED,
         "Console_Write_Completed", {"result"}, {"Result<Unit,String>"}, 1},
        {OM_SYSTEM_MESSAGE_FILE_READ, "File_Read", {"path", "reply_to"},
         {"String", "ActorRef"}, 2},
        {OM_SYSTEM_MESSAGE_FILE_READ_COMPLETED, "File_Read_Completed",
         {"result"}, {"Result<String,String>"}, 1},
        {OM_SYSTEM_MESSAGE_FILE_WRITE,
         "File_Write", {"path", "contents", "reply_to"},
         {"String", "String", "ActorRef"}, 3},
        {OM_SYSTEM_MESSAGE_FILE_WRITE_COMPLETED, "File_Write_Completed",
         {"result"}, {"Result<Unit,String>"}, 1},
        {OM_SYSTEM_MESSAGE_TIMER_AFTER,
         "Timer_After", {"delay_milliseconds", "message", "recipient"},
         {"Int", "Message", "ActorRef"}, 3}
    };
    if (builder == NULL || builder->owned_storage == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t schema_index = 0;
         schema_index < sizeof(definitions) / sizeof(definitions[0]);
         ++schema_index) {
        const size_t field_count = definitions[schema_index].field_count;
        uint32_t field_constants[3] = {0};
        for (size_t field_index = 0; field_index < field_count; ++field_index) {
            const char *name = definitions[schema_index].field_names[field_index];
            size_t name_length = 0;
            while (name[name_length] != '\0') {
                ++name_length;
            }
            const Om_Bytecode_Constant field_name = {
                .kind = OM_BYTECODE_CONSTANT_STRING,
                .as.string_value = {name, name_length}
            };
            const Om_Status constant_status = om_bytecode_builder_add_constant(
                builder, &field_name, &field_constants[field_index]);
            if (constant_status != OM_STATUS_OK) {
                return constant_status;
            }
        }
        uint32_t schema_index_out = 0;
        uint64_t schema_fingerprint = 0;
        const Om_Status fingerprint_status = fingerprint_system_schema(
            definitions[schema_index].schema_name,
            definitions[schema_index].field_types, field_count,
            &schema_fingerprint);
        if (fingerprint_status != OM_STATUS_OK) {
            return fingerprint_status;
        }
        const Om_Status status = om_bytecode_builder_add_schema(
            builder,
            (Om_Symbol_Id){definitions[schema_index].symbol_id},
            OM_BYTECODE_SCHEMA_MESSAGE,
            field_constants,
            field_count,
            schema_fingerprint,
            &schema_index_out);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

Om_Status om_bytecode_builder_finish(
    Om_Bytecode_Builder *builder,
    Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *verify_result
)
{
    Om_Bytecode_Storage *source = builder_storage(builder);
    if (source == NULL || image == NULL || image->owned_storage != NULL
        || source->active_function != UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    image->program_fingerprint = 0;
    image->interface_fingerprint = 0;
    Om_Bytecode_Image view = {0};
    om_bytecode_storage_bind_image(source, &view);
    Om_Bytecode_Storage *copy = NULL;
    Om_Status status = om_bytecode_storage_clone_image(&view, &copy);
    if (status != OM_STATUS_OK) {
        return status;
    }
    image->owned_storage = copy;
    om_bytecode_storage_bind_image(copy, image);
    status = om_bytecode_prepare_image(image, verify_result);
    if (status != OM_STATUS_OK || (verify_result != NULL && !verify_result->valid)) {
        om_bytecode_image_destroy(image);
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    if (verify_result == NULL) {
        Om_Bytecode_Verify_Result local = {0};
        status = om_bytecode_verify(image, &local);
        if (status != OM_STATUS_OK || !local.valid) {
            om_bytecode_image_destroy(image);
            return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
        }
    }
    return OM_STATUS_OK;
}

void om_bytecode_image_destroy(Om_Bytecode_Image *image)
{
    if (image == NULL) {
        return;
    }
    om_bytecode_storage_destroy(image->owned_storage);
    *image = (Om_Bytecode_Image){0};
}
