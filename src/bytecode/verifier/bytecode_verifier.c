#include "ombasic/bytecode/bytecode.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../bytecode_internal.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

static bool index_in_range(uint32_t index, size_t count)
{
    return (size_t)index < count;
}

void om_bytecode_set_verify_failure(
    Om_Bytecode_Verify_Result *result,
    Om_Bytecode_Verify_Code code,
    uint32_t function_index,
    uint32_t instruction_index
)
{
    if (result != NULL && result->code == OM_BYTECODE_VERIFY_OK) {
        result->valid = false;
        result->code = code;
        result->function_index = function_index;
        result->instruction_index = instruction_index;
    }
}

static bool verify_constant(const Om_Bytecode_Constant *constant)
{
    if (constant->kind > OM_BYTECODE_CONSTANT_STRING) {
        return false;
    }
    if (constant->kind == OM_BYTECODE_CONSTANT_DOWN_REASON) {
        return constant->as.down_reason_value <= 2;
    }
    if (constant->kind == OM_BYTECODE_CONSTANT_STRING) {
        return (constant->as.string_value.length == 0
                || constant->as.string_value.data != NULL)
            && om_utf8_validate((Om_Const_Byte_Slice){
                   (const uint8_t *)constant->as.string_value.data,
                   constant->as.string_value.length}) == OM_STATUS_OK;
    }
    return true;
}

static bool verify_source_map(const Om_Bytecode_Image *image,
                              Om_Bytecode_Verify_Result *result)
{
    uint32_t previous_end = 0;
    for (size_t index = 0; index < image->source_map_count; ++index) {
        const Om_Bytecode_Source_Map_Entry *entry = &image->source_map[index];
        if (entry->start_pc >= entry->end_pc
            || (size_t)entry->end_pc > image->instruction_count
            || entry->span.source.value == UINT32_MAX
            || entry->span.start > entry->span.end
            || (index > 0 && entry->start_pc < previous_end)) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_SOURCE_MAP, UINT32_MAX,
                entry->start_pc);
            return false;
        }
        previous_end = entry->end_pc;
    }
    return true;
}

static int compare_u32(const void *left, const void *right)
{
    const uint32_t left_value = *(const uint32_t *)left;
    const uint32_t right_value = *(const uint32_t *)right;
    return left_value < right_value ? -1 : left_value > right_value ? 1 : 0;
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t left_value = *(const uint64_t *)left;
    const uint64_t right_value = *(const uint64_t *)right;
    return left_value < right_value ? -1 : left_value > right_value ? 1 : 0;
}

static bool verify_actors(const Om_Bytecode_Image *image,
                          Om_Bytecode_Verify_Result *result,
                          uint32_t **actor_ids_out)
{
    *actor_ids_out = NULL;
    uint32_t *actor_ids = om_alloc_array(image->actor_count,
                                         sizeof(*actor_ids));
    uint64_t *handler_keys = om_alloc_array(image->handler_count,
                                            sizeof(*handler_keys));
    uint8_t *handler_owners = om_alloc_array(image->handler_count,
                                             sizeof(*handler_owners));
    if ((image->actor_count > 0 && actor_ids == NULL)
        || (image->handler_count > 0
            && (handler_keys == NULL || handler_owners == NULL))) {
        om_free(actor_ids);
        om_free(handler_keys);
        om_free(handler_owners);
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX, UINT32_MAX);
        return false;
    }
    if (handler_owners != NULL) {
        (void)memset(handler_owners, 0,
                     image->handler_count * sizeof(*handler_owners));
    }
    size_t key_count = 0;
    for (size_t index = 0; index < image->actor_count; ++index) {
        const Om_Bytecode_Actor *actor = &image->actors[index];
        const uint64_t handler_end = (uint64_t)actor->first_handler
            + (uint64_t)actor->handler_count;
        if (actor->actor_symbol.value == UINT32_MAX
            || (actor->initializer_function != UINT32_MAX
                && !index_in_range(actor->initializer_function,
                                   image->function_count))
            || handler_end > image->handler_count) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_ACTOR, UINT32_MAX,
                UINT32_MAX);
            om_free(actor_ids);
            om_free(handler_keys);
            om_free(handler_owners);
            return false;
        }
        actor_ids[index] = actor->actor_symbol.value;
        for (uint32_t handler_offset = 0;
             handler_offset < actor->handler_count; ++handler_offset) {
            const size_t handler_index =
                (size_t)actor->first_handler + handler_offset;
            const Om_Bytecode_Actor_Handler *handler =
                &image->handlers[handler_index];
            if (handler->message_symbol.value == UINT32_MAX
                || !index_in_range(handler->function_index,
                                   image->function_count)
                || handler_owners[handler_index] != 0) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_ACTOR, UINT32_MAX,
                    UINT32_MAX);
                om_free(actor_ids);
                om_free(handler_keys);
                om_free(handler_owners);
                return false;
            }
            handler_owners[handler_index] = 1;
            handler_keys[key_count++] =
                ((uint64_t)actor->actor_symbol.value << 32)
                | handler->message_symbol.value;
        }
    }
    for (size_t index = 0; index < image->handler_count; ++index) {
        if (handler_owners[index] == 0) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_ACTOR, UINT32_MAX,
                UINT32_MAX);
            om_free(actor_ids);
            om_free(handler_keys);
            om_free(handler_owners);
            return false;
        }
    }
    if (image->actor_count > 1) {
        qsort(actor_ids, image->actor_count, sizeof(*actor_ids), compare_u32);
        for (size_t index = 1; index < image->actor_count; ++index) {
            if (actor_ids[index - 1] == actor_ids[index]) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_ACTOR, UINT32_MAX,
                    UINT32_MAX);
                om_free(actor_ids);
                om_free(handler_keys);
                om_free(handler_owners);
                return false;
            }
        }
    }
    if (key_count > 1) {
        qsort(handler_keys, key_count, sizeof(*handler_keys), compare_u64);
        for (size_t index = 1; index < key_count; ++index) {
            if (handler_keys[index - 1] == handler_keys[index]) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_ACTOR, UINT32_MAX,
                    UINT32_MAX);
                om_free(actor_ids);
                om_free(handler_keys);
                om_free(handler_owners);
                return false;
            }
        }
    }
    om_free(handler_keys);
    om_free(handler_owners);
    *actor_ids_out = actor_ids;
    return true;
}

static bool actor_id_exists(const uint32_t *actor_ids, size_t actor_count,
                            uint32_t symbol_id)
{
    size_t low = 0;
    size_t high = actor_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (actor_ids[middle] < symbol_id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < actor_count && actor_ids[low] == symbol_id;
}

typedef struct Om_Bc_Schema_Index {
    uint32_t symbol_id;
    uint32_t field_count;
    Om_Bytecode_Schema_Kind kind;
} Om_Bc_Schema_Index;

typedef struct Om_Bc_Schema_Name_Key {
    uint32_t symbol_id;
    const char *name;
    size_t length;
} Om_Bc_Schema_Name_Key;

static int compare_schema_index(const void *left, const void *right)
{
    const Om_Bc_Schema_Index *left_value = left;
    const Om_Bc_Schema_Index *right_value = right;
    return left_value->symbol_id < right_value->symbol_id ? -1
        : left_value->symbol_id > right_value->symbol_id ? 1 : 0;
}

static int compare_schema_name_key(const void *left, const void *right)
{
    const Om_Bc_Schema_Name_Key *left_value = left;
    const Om_Bc_Schema_Name_Key *right_value = right;
    if (left_value->symbol_id != right_value->symbol_id) {
        return left_value->symbol_id < right_value->symbol_id ? -1 : 1;
    }
    const size_t common = left_value->length < right_value->length
        ? left_value->length : right_value->length;
    const int compared = common > 0
        ? memcmp(left_value->name, right_value->name, common) : 0;
    if (compared != 0) {
        return compared;
    }
    return left_value->length < right_value->length ? -1
        : left_value->length > right_value->length ? 1 : 0;
}

static const Om_Bc_Schema_Index *find_schema(
    const Om_Bc_Schema_Index *schemas,
    size_t schema_count,
    uint32_t symbol_id
)
{
    size_t low = 0;
    size_t high = schema_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (schemas[middle].symbol_id < symbol_id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < schema_count && schemas[low].symbol_id == symbol_id
        ? &schemas[low] : NULL;
}

static bool verify_schemas(
    const Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *result,
    Om_Bc_Schema_Index **schema_index_out
)
{
    *schema_index_out = NULL;
    Om_Bc_Schema_Index *schema_index = om_alloc_array(
        image->schema_count, sizeof(*schema_index));
    Om_Bc_Schema_Name_Key *name_keys = om_alloc_array(
        image->schema_field_count, sizeof(*name_keys));
    uint8_t *field_owners = om_alloc_array(
        image->schema_field_count, sizeof(*field_owners));
    if ((image->schema_count > 0 && schema_index == NULL)
        || (image->schema_field_count > 0
            && (name_keys == NULL || field_owners == NULL))) {
        om_free(schema_index);
        om_free(name_keys);
        om_free(field_owners);
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX, UINT32_MAX);
        return false;
    }
    if (field_owners != NULL) {
        (void)memset(field_owners, 0,
                     image->schema_field_count * sizeof(*field_owners));
    }
    size_t key_count = 0;
    for (size_t index = 0; index < image->schema_count; ++index) {
        const Om_Bytecode_Schema *schema = &image->schemas[index];
        const uint64_t field_end = (uint64_t)schema->first_field
            + schema->field_count;
        if (schema->symbol_id.value == UINT32_MAX
            || schema->kind > OM_BYTECODE_SCHEMA_MESSAGE
            || field_end > image->schema_field_count) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX,
                UINT32_MAX);
            om_free(schema_index);
            om_free(name_keys);
            om_free(field_owners);
            return false;
        }
        schema_index[index] = (Om_Bc_Schema_Index){
            .symbol_id = schema->symbol_id.value,
            .field_count = schema->field_count,
            .kind = schema->kind
        };
        for (uint32_t offset = 0; offset < schema->field_count; ++offset) {
            const size_t field_index = (size_t)schema->first_field + offset;
            const Om_Bytecode_Schema_Field *field =
                &image->schema_fields[field_index];
            if (field_owners[field_index] != 0
                || !index_in_range(field->name_constant,
                                   image->constant_count)
                || image->constants[field->name_constant].kind
                    != OM_BYTECODE_CONSTANT_STRING) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX,
                    UINT32_MAX);
                om_free(schema_index);
                om_free(name_keys);
                om_free(field_owners);
                return false;
            }
            field_owners[field_index] = 1;
            const Om_String_View name =
                image->constants[field->name_constant].as.string_value;
            name_keys[key_count++] = (Om_Bc_Schema_Name_Key){
                .symbol_id = schema->symbol_id.value,
                .name = name.data,
                .length = name.length
            };
        }
    }
    for (size_t index = 0; index < image->schema_field_count; ++index) {
        if (field_owners[index] == 0) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX,
                UINT32_MAX);
            om_free(schema_index);
            om_free(name_keys);
            om_free(field_owners);
            return false;
        }
    }
    if (image->schema_count > 1) {
        qsort(schema_index, image->schema_count, sizeof(*schema_index),
              compare_schema_index);
        for (size_t index = 1; index < image->schema_count; ++index) {
            if (schema_index[index - 1].symbol_id
                == schema_index[index].symbol_id) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX,
                    UINT32_MAX);
                om_free(schema_index);
                om_free(name_keys);
                om_free(field_owners);
                return false;
            }
        }
    }
    if (key_count > 1) {
        qsort(name_keys, key_count, sizeof(*name_keys), compare_schema_name_key);
        for (size_t index = 1; index < key_count; ++index) {
            if (compare_schema_name_key(&name_keys[index - 1],
                                       &name_keys[index]) == 0) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX,
                    UINT32_MAX);
                om_free(schema_index);
                om_free(name_keys);
                om_free(field_owners);
                return false;
            }
        }
    }
    om_free(name_keys);
    om_free(field_owners);
    *schema_index_out = schema_index;
    return true;
}

static bool validate_instruction_operands(
    const Om_Bytecode_Image *image,
    const Om_Bc_Schema_Index *schemas,
    size_t schema_count,
    const uint32_t *actor_ids,
    const uint32_t *function_state_counts,
    const uint8_t *function_actor_roles,
    uint32_t function_index,
    uint32_t pc,
    uint8_t *instruction_owners,
    Om_Bytecode_Verify_Result *result
)
{
    const Om_Bytecode_Instruction *instruction = &image->instructions[pc];
    const Om_Bytecode_Function *function = &image->functions[function_index];
    const uint32_t opcode = (uint32_t)instruction->opcode;
    bool valid = true;
    if (instruction_owners[pc] != 0) {
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_FUNCTION, function_index, pc);
        return false;
    }
    instruction_owners[pc] = 1;
    if (opcode >= OM_BC_OPCODE_COUNT) {
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_OPCODE, function_index, pc);
        return false;
    }
    bool uses_a = false;
    bool uses_b = false;
    switch (instruction->opcode) {
    case OM_BC_LOAD_CONST:
    case OM_BC_LOAD_LOCAL:
    case OM_BC_STORE_LOCAL:
    case OM_BC_JUMP:
    case OM_BC_JUMP_IF_FALSE:
    case OM_BC_JUMP_IF_TRUE:
    case OM_BC_LOAD_FUNCTION:
    case OM_BC_GET_FIELD:
    case OM_BC_WITH_FIELD:
    case OM_BC_MAKE_LIST:
    case OM_BC_MAKE_MAP:
    case OM_BC_BUILD_STRING:
    case OM_BC_LOAD_STATE:
    case OM_BC_STORE_STATE:
    case OM_BC_LOAD_SYSTEM_ACTOR:
        uses_a = true;
        break;
    case OM_BC_CALL_INDIRECT:
        uses_b = true;
        break;
    case OM_BC_CALL:
    case OM_BC_MAKE_RECORD:
    case OM_BC_MAKE_MESSAGE:
    case OM_BC_SPAWN:
    case OM_BC_ITER_NEXT:
        uses_a = true;
        uses_b = true;
        break;
    case OM_BC_NOP:
    case OM_BC_LOAD_UNIT:
    case OM_BC_POP:
    case OM_BC_DUP:
    case OM_BC_ADD_INT:
    case OM_BC_SUB_INT:
    case OM_BC_MUL_INT:
    case OM_BC_DIV_INT:
    case OM_BC_MOD_INT:
    case OM_BC_NEG_INT:
    case OM_BC_ADD_FLOAT:
    case OM_BC_SUB_FLOAT:
    case OM_BC_MUL_FLOAT:
    case OM_BC_DIV_FLOAT:
    case OM_BC_NEG_FLOAT:
    case OM_BC_NOT_BOOL:
    case OM_BC_EQ:
    case OM_BC_NE:
    case OM_BC_LT:
    case OM_BC_LE:
    case OM_BC_GT:
    case OM_BC_GE:
    case OM_BC_RETURN:
    case OM_BC_MAKE_OK:
    case OM_BC_MAKE_ERROR:
    case OM_BC_IS_OK:
    case OM_BC_UNWRAP_ERROR:
    case OM_BC_GET_INDEX:
    case OM_BC_TO_STRING:
    case OM_BC_CHECK_RESULT:
    case OM_BC_UNWRAP_OK:
    case OM_BC_RETURN_ERROR:
    case OM_BC_SEND:
    case OM_BC_WATCH:
    case OM_BC_STOP:
    case OM_BC_SELF:
    case OM_BC_FOR_RANGE_TEST:
    case OM_BC_ITER_INIT:
        break;
    case OM_BC_OPCODE_COUNT:
        break;
    }
    if ((!uses_a && instruction->operand_a != 0)
        || (!uses_b && instruction->operand_b != 0)
        || instruction->immediate != 0) {
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_OPERAND, function_index, pc);
        return false;
    }
    switch (instruction->opcode) {
    case OM_BC_LOAD_CONST:
        valid = index_in_range(instruction->operand_a, image->constant_count);
        break;
    case OM_BC_LOAD_SYSTEM_ACTOR:
        valid = instruction->operand_a < OM_BYTECODE_SYSTEM_ACTOR_COUNT;
        break;
    case OM_BC_LOAD_LOCAL:
    case OM_BC_STORE_LOCAL:
        valid = instruction->operand_a < function->local_count;
        break;
    case OM_BC_JUMP:
    case OM_BC_JUMP_IF_FALSE:
    case OM_BC_JUMP_IF_TRUE:
        valid = instruction->operand_a >= function->entry_pc
            && instruction->operand_a < function->end_pc;
        if (!valid) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_JUMP, function_index, pc);
            return false;
        }
        break;
    case OM_BC_CALL:
        valid = index_in_range(instruction->operand_a, image->function_count);
        if (valid) {
            const Om_Bytecode_Function *callee =
                &image->functions[instruction->operand_a];
            valid = instruction->operand_b == callee->parameter_count
                || ((callee->flags & (OM_BYTECODE_FUNCTION_EXTERNAL
                                      | OM_BYTECODE_FUNCTION_VARIADIC))
                    == (OM_BYTECODE_FUNCTION_EXTERNAL
                        | OM_BYTECODE_FUNCTION_VARIADIC));
        }
        break;
    case OM_BC_CALL_INDIRECT:
        break;
    case OM_BC_LOAD_FUNCTION:
        valid = index_in_range(instruction->operand_a, image->function_count);
        break;
    case OM_BC_MAKE_RECORD:
    case OM_BC_MAKE_MESSAGE: {
        const Om_Bc_Schema_Index *schema =
            find_schema(schemas, schema_count, instruction->operand_a);
        const Om_Bytecode_Schema_Kind expected =
            instruction->opcode == OM_BC_MAKE_RECORD
                ? OM_BYTECODE_SCHEMA_RECORD : OM_BYTECODE_SCHEMA_MESSAGE;
        valid = schema != NULL && schema->kind == expected
            && schema->field_count == instruction->operand_b;
        break;
    }
    case OM_BC_GET_FIELD:
    case OM_BC_WITH_FIELD:
        valid = index_in_range(instruction->operand_a, image->constant_count)
            && image->constants[instruction->operand_a].kind
                == OM_BYTECODE_CONSTANT_STRING;
        break;
    case OM_BC_ITER_NEXT:
        valid = instruction->operand_a < function->local_count
            && instruction->operand_b < function->local_count;
        break;
    case OM_BC_SPAWN: {
        valid = instruction->operand_b <= 1;
        valid = valid && actor_id_exists(actor_ids, image->actor_count,
                                         instruction->operand_a);
        break;
    }
    case OM_BC_LOAD_STATE:
    case OM_BC_STORE_STATE: {
        valid = function_actor_roles[function_index] != 0
            && instruction->operand_a < function_state_counts[function_index];
        break;
    }
    case OM_BC_NOP:
    case OM_BC_LOAD_UNIT:
    case OM_BC_POP:
    case OM_BC_DUP:
    case OM_BC_ADD_INT:
    case OM_BC_SUB_INT:
    case OM_BC_MUL_INT:
    case OM_BC_DIV_INT:
    case OM_BC_MOD_INT:
    case OM_BC_NEG_INT:
    case OM_BC_ADD_FLOAT:
    case OM_BC_SUB_FLOAT:
    case OM_BC_MUL_FLOAT:
    case OM_BC_DIV_FLOAT:
    case OM_BC_NEG_FLOAT:
    case OM_BC_NOT_BOOL:
    case OM_BC_EQ:
    case OM_BC_NE:
    case OM_BC_LT:
    case OM_BC_LE:
    case OM_BC_GT:
    case OM_BC_GE:
    case OM_BC_RETURN:
    case OM_BC_MAKE_OK:
    case OM_BC_MAKE_ERROR:
    case OM_BC_IS_OK:
    case OM_BC_UNWRAP_ERROR:
    case OM_BC_MAKE_LIST:
    case OM_BC_MAKE_MAP:
    case OM_BC_GET_INDEX:
    case OM_BC_BUILD_STRING:
    case OM_BC_TO_STRING:
    case OM_BC_CHECK_RESULT:
    case OM_BC_UNWRAP_OK:
    case OM_BC_RETURN_ERROR:
    case OM_BC_SEND:
    case OM_BC_WATCH:
    case OM_BC_STOP:
    case OM_BC_SELF:
    case OM_BC_FOR_RANGE_TEST:
    case OM_BC_ITER_INIT:
        break;
    case OM_BC_OPCODE_COUNT:
        valid = false;
        break;
    }
    if (!valid) {
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_OPERAND, function_index, pc);
    }
    return valid;
}

typedef struct Om_Bytecode_Stack_Effect {
    int64_t required;
    int64_t delta;
    bool terminal;
    bool conditional_branch;
    bool unconditional_branch;
} Om_Bytecode_Stack_Effect;

static bool instruction_stack_effect(
    const Om_Bytecode_Image *image,
    const Om_Bytecode_Instruction *instruction,
    Om_Bytecode_Stack_Effect *effect
)
{
    *effect = (Om_Bytecode_Stack_Effect){0};
    switch (instruction->opcode) {
    case OM_BC_NOP:
    case OM_BC_JUMP:
        effect->unconditional_branch = instruction->opcode == OM_BC_JUMP;
        return true;
    case OM_BC_LOAD_CONST:
    case OM_BC_LOAD_UNIT:
    case OM_BC_LOAD_LOCAL:
    case OM_BC_LOAD_FUNCTION:
    case OM_BC_SELF:
    case OM_BC_LOAD_SYSTEM_ACTOR:
        effect->delta = 1;
        return true;
    case OM_BC_STORE_LOCAL:
    case OM_BC_POP:
    case OM_BC_WATCH:
    case OM_BC_STOP:
    case OM_BC_STORE_STATE:
        effect->required = 1;
        effect->delta = -1;
        return true;
    case OM_BC_DUP:
    case OM_BC_NEG_INT:
    case OM_BC_NEG_FLOAT:
    case OM_BC_NOT_BOOL:
    case OM_BC_TO_STRING:
    case OM_BC_UNWRAP_OK:
    case OM_BC_MAKE_OK:
    case OM_BC_MAKE_ERROR:
    case OM_BC_IS_OK:
    case OM_BC_UNWRAP_ERROR:
    case OM_BC_ITER_INIT:
        effect->required = 1;
        effect->delta = instruction->opcode == OM_BC_DUP ? 1 : 0;
        return true;
    case OM_BC_ADD_INT:
    case OM_BC_SUB_INT:
    case OM_BC_MUL_INT:
    case OM_BC_DIV_INT:
    case OM_BC_MOD_INT:
    case OM_BC_ADD_FLOAT:
    case OM_BC_SUB_FLOAT:
    case OM_BC_MUL_FLOAT:
    case OM_BC_DIV_FLOAT:
    case OM_BC_EQ:
    case OM_BC_NE:
    case OM_BC_LT:
    case OM_BC_LE:
    case OM_BC_GT:
    case OM_BC_GE:
    case OM_BC_GET_INDEX:
    case OM_BC_WITH_FIELD:
        effect->required = 2;
        effect->delta = -1;
        return true;
    case OM_BC_GET_FIELD:
        effect->required = 1;
        return true;
    case OM_BC_JUMP_IF_FALSE:
    case OM_BC_JUMP_IF_TRUE:
        effect->required = 1;
        effect->delta = -1;
        effect->conditional_branch = true;
        return true;
    case OM_BC_CALL:
        effect->required = instruction->operand_b;
        effect->delta = 1 - (int64_t)instruction->operand_b;
        return true;
    case OM_BC_CALL_INDIRECT:
        effect->required = (int64_t)instruction->operand_b + 1;
        effect->delta = -(int64_t)instruction->operand_b;
        return true;
    case OM_BC_RETURN:
    case OM_BC_RETURN_ERROR:
        effect->required = 1;
        effect->delta = -1;
        effect->terminal = true;
        return true;
    case OM_BC_MAKE_RECORD:
    case OM_BC_MAKE_MESSAGE:
    case OM_BC_MAKE_LIST:
        effect->required = instruction->operand_b;
        effect->delta = 1 - (int64_t)instruction->operand_b;
        if (instruction->opcode == OM_BC_MAKE_LIST) {
            effect->required = instruction->operand_a;
            effect->delta = 1 - (int64_t)instruction->operand_a;
        }
        return true;
    case OM_BC_MAKE_MAP:
        if (instruction->operand_a > (uint32_t)(INT64_MAX / 2)) {
            return false;
        }
        effect->required = (int64_t)instruction->operand_a * 2;
        effect->delta = 1 - effect->required;
        return true;
    case OM_BC_BUILD_STRING:
        effect->required = instruction->operand_a;
        effect->delta = 1 - (int64_t)instruction->operand_a;
        return true;
    case OM_BC_CHECK_RESULT:
        effect->required = 1;
        effect->delta = 1;
        return true;
    case OM_BC_SPAWN:
        effect->required = instruction->operand_b;
        effect->delta = 1 - (int64_t)instruction->operand_b;
        return true;
    case OM_BC_SEND:
        effect->required = 2;
        effect->delta = -2;
        return true;
    case OM_BC_FOR_RANGE_TEST:
        effect->required = 3;
        effect->delta = -2;
        return true;
    case OM_BC_ITER_NEXT:
        effect->delta = 1;
        return true;
    case OM_BC_LOAD_STATE:
        effect->delta = 1;
        return true;
    case OM_BC_OPCODE_COUNT:
        return false;
    }
    (void)image;
    return false;
}

static bool analyze_function_stack(
    const Om_Bytecode_Image *image,
    uint32_t function_index,
    bool set_maximum,
    Om_Bytecode_Verify_Result *result
)
{
    Om_Bytecode_Function *mutable_function =
        (Om_Bytecode_Function *)(uintptr_t)&image->functions[function_index];
    const Om_Bytecode_Function *function = &image->functions[function_index];
    const size_t range_length = (size_t)(function->end_pc - function->entry_pc);
    int64_t *heights = om_alloc_array(range_length, sizeof(*heights));
    uint32_t *queue = om_alloc_array(range_length, sizeof(*queue));
    if (range_length != 0 && (heights == NULL || queue == NULL)) {
        om_free(heights);
        om_free(queue);
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_IMAGE,
            function_index, function->entry_pc);
        return false;
    }
    for (size_t index = 0; index < range_length; ++index) {
        heights[index] = -1;
    }
    size_t queue_head = 0;
    size_t queue_tail = 0;
    heights[0] = 0;
    queue[queue_tail++] = function->entry_pc;
    int64_t maximum = 0;
    bool valid = true;
    while (queue_head < queue_tail && valid) {
        const uint32_t pc = queue[queue_head++];
        const Om_Bytecode_Instruction *instruction = &image->instructions[pc];
        const int64_t before = heights[pc - function->entry_pc];
        Om_Bytecode_Stack_Effect effect = {0};
        if (!instruction_stack_effect(image, instruction, &effect)
            || before < effect.required) {
            om_bytecode_set_verify_failure(
                result, before < effect.required
                    ? OM_BYTECODE_VERIFY_STACK_UNDERFLOW
                    : OM_BYTECODE_VERIFY_INVALID_OPCODE,
                function_index, pc);
            valid = false;
            break;
        }
        const int64_t after = before + effect.delta;
        if (after < 0 || after > UINT32_MAX) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_STACK_LIMIT, function_index, pc);
            valid = false;
            break;
        }
        if (instruction->opcode == OM_BC_RETURN
            || instruction->opcode == OM_BC_RETURN_ERROR) {
            if (before != 1) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_STACK_MISMATCH,
                    function_index, pc);
                valid = false;
                break;
            }
        }
        if (after > maximum) {
            maximum = after;
        }
        uint32_t successors[2] = {0, 0};
        size_t successor_count = 0;
        if (effect.unconditional_branch) {
            successors[successor_count++] = instruction->operand_a;
        } else if (effect.conditional_branch) {
            successors[successor_count++] = instruction->operand_a;
            if (pc + 1 < function->end_pc) {
                successors[successor_count++] = pc + 1;
            } else {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_FUNCTION,
                    function_index, pc);
                valid = false;
                break;
            }
        } else if (!effect.terminal) {
            if (pc + 1 >= function->end_pc) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_FUNCTION,
                    function_index, pc);
                valid = false;
                break;
            }
            successors[successor_count++] = pc + 1;
        }
        for (size_t successor = 0; successor < successor_count; ++successor) {
            const uint32_t target = successors[successor];
            if (target < function->entry_pc || target >= function->end_pc) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_JUMP,
                    function_index, pc);
                valid = false;
                break;
            }
            const size_t target_offset =
                (size_t)(target - function->entry_pc);
            if (heights[target_offset] == -1) {
                heights[target_offset] = after;
                if (queue_tail >= range_length) {
                    om_bytecode_set_verify_failure(
                        result, OM_BYTECODE_VERIFY_INVALID_IMAGE,
                        function_index, pc);
                    valid = false;
                    break;
                }
                queue[queue_tail++] = target;
            } else if (heights[target_offset] != after) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_STACK_MISMATCH,
                    function_index, target);
                valid = false;
                break;
            }
        }
    }
    if (valid && set_maximum) {
        if (maximum > UINT32_MAX) {
            valid = false;
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_STACK_LIMIT,
                function_index, function->entry_pc);
        } else {
            mutable_function->max_stack = (uint32_t)maximum;
        }
    } else if (valid && maximum > function->max_stack) {
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_STACK_LIMIT,
            function_index, function->entry_pc);
        valid = false;
    }
    om_free(heights);
    om_free(queue);
    return valid;
}

static bool build_actor_function_maps(
    const Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *result,
    uint32_t **state_counts_out,
    uint8_t **roles_out
)
{
    *state_counts_out = NULL;
    *roles_out = NULL;
    uint32_t *state_counts = om_alloc_array(
        image->function_count, sizeof(*state_counts));
    uint8_t *roles = om_alloc_array(image->function_count, sizeof(*roles));
    uint32_t *owners = om_alloc_array(image->function_count, sizeof(*owners));
    if (image->function_count > 0
        && (state_counts == NULL || roles == NULL || owners == NULL)) {
        om_free(state_counts);
        om_free(roles);
        om_free(owners);
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX, UINT32_MAX);
        return false;
    }
    for (size_t index = 0; index < image->function_count; ++index) {
        state_counts[index] = 0;
        roles[index] = 0;
        owners[index] = UINT32_MAX;
    }
    for (size_t actor_index = 0; actor_index < image->actor_count; ++actor_index) {
        const Om_Bytecode_Actor *actor = &image->actors[actor_index];
        if (actor->initializer_function != UINT32_MAX) {
            const uint32_t function_index = actor->initializer_function;
            if (owners[function_index] != UINT32_MAX) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_ACTOR, UINT32_MAX,
                    UINT32_MAX);
                om_free(state_counts);
                om_free(roles);
                om_free(owners);
                return false;
            }
            owners[function_index] = (uint32_t)actor_index;
            state_counts[function_index] = actor->state_count;
            roles[function_index] = 1;
        }
        const uint64_t handler_end =
            (uint64_t)actor->first_handler + actor->handler_count;
        for (uint64_t handler_index = actor->first_handler;
             handler_index < handler_end; ++handler_index) {
            const uint32_t function_index =
                image->handlers[handler_index].function_index;
            if (owners[function_index] != UINT32_MAX) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_ACTOR, UINT32_MAX,
                    UINT32_MAX);
                om_free(state_counts);
                om_free(roles);
                om_free(owners);
                return false;
            }
            owners[function_index] = (uint32_t)actor_index;
            state_counts[function_index] = actor->state_count;
            roles[function_index] = 2;
        }
    }
    om_free(owners);
    *state_counts_out = state_counts;
    *roles_out = roles;
    return true;
}

static bool validate_image(Om_Bytecode_Image *image,
                           bool prepare,
                           Om_Bytecode_Verify_Result *result)
{
    if (image == NULL
        || image->constant_count > UINT32_MAX
        || image->function_count > UINT32_MAX
        || image->instruction_count > UINT32_MAX
        || image->source_map_count > UINT32_MAX
        || image->actor_count > UINT32_MAX
        || image->handler_count > UINT32_MAX
        || image->schema_count > UINT32_MAX
        || image->schema_field_count > UINT32_MAX
        || (image->constant_count > 0 && image->constants == NULL)
        || (image->function_count > 0 && image->functions == NULL)
        || (image->instruction_count > 0 && image->instructions == NULL)
        || (image->source_map_count > 0 && image->source_map == NULL)
        || (image->actor_count > 0 && image->actors == NULL)
        || (image->handler_count > 0 && image->handlers == NULL)
        || (image->schema_count > 0 && image->schemas == NULL)
        || (image->schema_field_count > 0 && image->schema_fields == NULL)) {
        om_bytecode_set_verify_failure(
            result, OM_BYTECODE_VERIFY_INVALID_IMAGE, UINT32_MAX, UINT32_MAX);
        return true;
    }
    for (size_t index = 0; index < image->constant_count; ++index) {
        if (!verify_constant(&image->constants[index])) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_CONSTANT, UINT32_MAX,
                UINT32_MAX);
            return true;
        }
    }
    if (!verify_source_map(image, result)) {
        return true;
    }
    uint32_t *actor_ids = NULL;
    if (!verify_actors(image, result, &actor_ids)) {
        return true;
    }
    uint32_t *function_state_counts = NULL;
    uint8_t *function_actor_roles = NULL;
    if (!build_actor_function_maps(image, result, &function_state_counts,
                                   &function_actor_roles)) {
        om_free(actor_ids);
        return true;
    }
    Om_Bc_Schema_Index *schemas = NULL;
    if (!verify_schemas(image, result, &schemas)) {
        om_free(actor_ids);
        om_free(function_state_counts);
        om_free(function_actor_roles);
        return true;
    }
    uint8_t *owners = om_alloc_array(image->instruction_count, sizeof(*owners));
    if (image->instruction_count > 0 && owners == NULL) {
        om_free(actor_ids);
        om_free(function_state_counts);
        om_free(function_actor_roles);
        om_free(schemas);
        return false;
    }
    if (owners != NULL) {
        (void)memset(owners, 0, image->instruction_count * sizeof(*owners));
    }
    for (size_t index = 0; index < image->function_count; ++index) {
        const Om_Bytecode_Function *function = &image->functions[index];
        const bool external =
            (function->flags & OM_BYTECODE_FUNCTION_EXTERNAL) != 0;
        const bool name_valid =
            index_in_range(function->name_constant, image->constant_count)
            && image->constants[function->name_constant].kind
                == OM_BYTECODE_CONSTANT_STRING;
        const uint32_t known_flags = OM_BYTECODE_FUNCTION_ACTION
            | OM_BYTECODE_FUNCTION_RECEIVE | OM_BYTECODE_FUNCTION_EXTERNAL
            | OM_BYTECODE_FUNCTION_VARIADIC;
        const uint8_t actor_role = function_actor_roles[index];
        const bool actor_role_valid = actor_role == 0
            || (actor_role == 1
                && (function->flags & OM_BYTECODE_FUNCTION_ACTION) != 0)
            || (actor_role == 2
                && (function->flags & OM_BYTECODE_FUNCTION_RECEIVE) != 0);
        if (!name_valid || (function->flags & ~known_flags) != 0
            || function->local_count < function->parameter_count
            || !actor_role_valid
            || (actor_role != 0
                && (function->flags & OM_BYTECODE_FUNCTION_EXTERNAL) != 0)) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_FUNCTION,
                (uint32_t)index, UINT32_MAX);
            om_free(owners);
            om_free(actor_ids);
            om_free(function_state_counts);
            om_free(function_actor_roles);
            om_free(schemas);
            return true;
        }
        if (external) {
            if (function->entry_pc != 0 || function->end_pc != 0
                || function->max_stack != 0) {
                om_bytecode_set_verify_failure(
                    result, OM_BYTECODE_VERIFY_INVALID_FUNCTION,
                    (uint32_t)index, UINT32_MAX);
                om_free(owners);
                om_free(actor_ids);
                om_free(function_state_counts);
                om_free(function_actor_roles);
                om_free(schemas);
                return true;
            }
            continue;
        }
        if (function->entry_pc >= function->end_pc
            || (size_t)function->end_pc > image->instruction_count) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_FUNCTION,
                (uint32_t)index, UINT32_MAX);
            om_free(owners);
            om_free(actor_ids);
            om_free(function_state_counts);
            om_free(function_actor_roles);
            om_free(schemas);
            return true;
        }
        for (uint32_t pc = function->entry_pc; pc < function->end_pc; ++pc) {
            if (!validate_instruction_operands(
                    image, schemas, image->schema_count,
                    actor_ids, function_state_counts, function_actor_roles,
                    (uint32_t)index, pc, owners, result)) {
                om_free(owners);
                om_free(actor_ids);
                om_free(function_state_counts);
                om_free(function_actor_roles);
                om_free(schemas);
                return true;
            }
        }
    }
    for (size_t pc = 0; pc < image->instruction_count; ++pc) {
        if (owners[pc] == 0) {
            om_bytecode_set_verify_failure(
                result, OM_BYTECODE_VERIFY_INVALID_FUNCTION, UINT32_MAX,
                (uint32_t)pc);
            om_free(owners);
            om_free(actor_ids);
            om_free(function_state_counts);
            om_free(function_actor_roles);
            om_free(schemas);
            return true;
        }
    }
    om_free(owners);
    for (size_t index = 0; index < image->function_count; ++index) {
        if ((image->functions[index].flags
             & OM_BYTECODE_FUNCTION_EXTERNAL) == 0
            && !analyze_function_stack(image, (uint32_t)index, prepare, result)) {
            om_free(actor_ids);
            om_free(function_state_counts);
            om_free(function_actor_roles);
            om_free(schemas);
            return true;
        }
    }
    om_free(actor_ids);
    om_free(function_state_counts);
    om_free(function_actor_roles);
    om_free(schemas);
    return true;
}

static Om_Status run_validation(Om_Bytecode_Image *image,
                                bool prepare,
                                Om_Bytecode_Verify_Result *result)
{
    if (result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = (Om_Bytecode_Verify_Result){
        .valid = false, .code = OM_BYTECODE_VERIFY_OK,
        .function_index = UINT32_MAX, .instruction_index = UINT32_MAX
    };
    if (!validate_image(image, prepare, result)) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    result->valid = result->code == OM_BYTECODE_VERIFY_OK;
    return OM_STATUS_OK;
}

Om_Status om_bytecode_prepare_image(
    Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *result
)
{
    Om_Bytecode_Verify_Result local = {0};
    return run_validation(image, true, result != NULL ? result : &local);
}

Om_Status om_bytecode_verify(
    const Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *result
)
{
    if (image == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return run_validation((Om_Bytecode_Image *)(uintptr_t)image,
                          false, result);
}

const char *om_bytecode_verify_code_name(Om_Bytecode_Verify_Code code)
{
    switch (code) {
    case OM_BYTECODE_VERIFY_OK: return "OK";
    case OM_BYTECODE_VERIFY_INVALID_IMAGE: return "INVALID_IMAGE";
    case OM_BYTECODE_VERIFY_INVALID_CONSTANT: return "INVALID_CONSTANT";
    case OM_BYTECODE_VERIFY_INVALID_FUNCTION: return "INVALID_FUNCTION";
    case OM_BYTECODE_VERIFY_INVALID_OPCODE: return "INVALID_OPCODE";
    case OM_BYTECODE_VERIFY_INVALID_OPERAND: return "INVALID_OPERAND";
    case OM_BYTECODE_VERIFY_INVALID_JUMP: return "INVALID_JUMP";
    case OM_BYTECODE_VERIFY_STACK_UNDERFLOW: return "STACK_UNDERFLOW";
    case OM_BYTECODE_VERIFY_STACK_MISMATCH: return "STACK_MISMATCH";
    case OM_BYTECODE_VERIFY_STACK_LIMIT: return "STACK_LIMIT";
    case OM_BYTECODE_VERIFY_INVALID_SOURCE_MAP: return "INVALID_SOURCE_MAP";
    case OM_BYTECODE_VERIFY_INVALID_ACTOR: return "INVALID_ACTOR";
    }
    return "UNKNOWN";
}

const char *om_bytecode_opcode_name(Om_Bytecode_Opcode opcode)
{
    static const char *const names[OM_BC_OPCODE_COUNT] = {
        "NOP", "LOAD_CONST", "LOAD_UNIT", "LOAD_LOCAL", "STORE_LOCAL",
        "POP", "DUP", "ADD_INT", "SUB_INT", "MUL_INT", "DIV_INT",
        "MOD_INT", "NEG_INT", "ADD_FLOAT", "SUB_FLOAT", "MUL_FLOAT",
        "DIV_FLOAT", "NEG_FLOAT", "NOT_BOOL", "EQ", "NE", "LT", "LE",
        "GT", "GE", "JUMP", "JUMP_IF_FALSE", "JUMP_IF_TRUE", "CALL",
        "CALL_INDIRECT", "LOAD_FUNCTION", "RETURN", "MAKE_RECORD",
        "MAKE_MESSAGE", "MAKE_OK", "MAKE_ERROR", "IS_OK", "UNWRAP_ERROR",
        "GET_FIELD", "WITH_FIELD", "MAKE_LIST", "MAKE_MAP",
        "GET_INDEX", "BUILD_STRING", "TO_STRING", "CHECK_RESULT",
        "UNWRAP_OK", "RETURN_ERROR", "SPAWN", "SEND", "WATCH", "STOP",
        "SELF", "FOR_RANGE_TEST", "ITER_INIT", "ITER_NEXT", "LOAD_STATE",
        "STORE_STATE", "LOAD_SYSTEM_ACTOR"
    };
    return opcode >= 0 && opcode < OM_BC_OPCODE_COUNT
        ? names[opcode] : "UNKNOWN";
}
