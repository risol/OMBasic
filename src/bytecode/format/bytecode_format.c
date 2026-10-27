#include "ombasic/bytecode/bytecode.h"

#include <float.h>
#include <string.h>

#include "../bytecode_internal.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

typedef struct Om_Bc_Writer {
    uint8_t *data;
    size_t length;
    size_t capacity;
} Om_Bc_Writer;

typedef struct Om_Bc_Reader {
    const uint8_t *data;
    size_t length;
    size_t offset;
} Om_Bc_Reader;

static bool host_supports_binary64(void)
{
    return sizeof(double) == sizeof(uint64_t)
        && FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024;
}

static bool checked_add_size(size_t left, size_t right, size_t *out)
{
    if (left > SIZE_MAX - right) {
        return false;
    }
    *out = left + right;
    return true;
}

static bool checked_mul_size(size_t left, size_t right, size_t *out)
{
    if (right != 0 && left > SIZE_MAX / right) {
        return false;
    }
    *out = left * right;
    return true;
}

static Om_Status writer_reserve(Om_Bc_Writer *writer, size_t extra)
{
    size_t needed = 0;
    if (!checked_add_size(writer->length, extra, &needed)
        || needed > OM_BYTECODE_DEFAULT_MAX_BYTES) {
        return OM_STATUS_OVERFLOW;
    }
    if (needed <= writer->capacity) {
        return OM_STATUS_OK;
    }
    size_t next = writer->capacity == 0 ? 256 : writer->capacity;
    while (next < needed) {
        if (next > OM_BYTECODE_DEFAULT_MAX_BYTES / 2) {
            next = OM_BYTECODE_DEFAULT_MAX_BYTES;
            break;
        }
        next *= 2;
    }
    uint8_t *grown = om_realloc_array(writer->data, next, sizeof(*grown));
    if (grown == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    writer->data = grown;
    writer->capacity = next;
    return OM_STATUS_OK;
}

static Om_Status writer_bytes(Om_Bc_Writer *writer,
                              const void *bytes,
                              size_t length)
{
    if (length > 0 && bytes == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = writer_reserve(writer, length);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (length > 0) {
        (void)memcpy(writer->data + writer->length, bytes, length);
    }
    writer->length += length;
    return OM_STATUS_OK;
}

static Om_Status writer_u8(Om_Bc_Writer *writer, uint8_t value)
{
    return writer_bytes(writer, &value, sizeof(value));
}

static Om_Status writer_u16(Om_Bc_Writer *writer, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)value, (uint8_t)(value >> 8)
    };
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static Om_Status writer_u32(Om_Bc_Writer *writer, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
        (uint8_t)(value >> 24)
    };
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static Om_Status writer_u64(Om_Bc_Writer *writer, uint64_t value)
{
    const uint8_t bytes[8] = {
        (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
        (uint8_t)(value >> 24), (uint8_t)(value >> 32),
        (uint8_t)(value >> 40), (uint8_t)(value >> 48),
        (uint8_t)(value >> 56)
    };
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static Om_Status reader_bytes(Om_Bc_Reader *reader,
                              void *destination,
                              size_t length)
{
    if (length > reader->length - reader->offset
        || (length > 0 && destination == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (length > 0) {
        (void)memcpy(destination, reader->data + reader->offset, length);
    }
    reader->offset += length;
    return OM_STATUS_OK;
}

static Om_Status reader_u8(Om_Bc_Reader *reader, uint8_t *value)
{
    return reader_bytes(reader, value, sizeof(*value));
}

static Om_Status reader_u16(Om_Bc_Reader *reader, uint16_t *value)
{
    uint8_t bytes[2] = {0};
    const Om_Status status = reader_bytes(reader, bytes, sizeof(bytes));
    if (status == OM_STATUS_OK) {
        *value = (uint16_t)((uint16_t)bytes[0]
                            | ((uint16_t)bytes[1] << 8));
    }
    return status;
}

static Om_Status reader_u32(Om_Bc_Reader *reader, uint32_t *value)
{
    uint8_t bytes[4] = {0};
    const Om_Status status = reader_bytes(reader, bytes, sizeof(bytes));
    if (status == OM_STATUS_OK) {
        *value = (uint32_t)bytes[0]
            | ((uint32_t)bytes[1] << 8)
            | ((uint32_t)bytes[2] << 16)
            | ((uint32_t)bytes[3] << 24);
    }
    return status;
}

static Om_Status reader_u64(Om_Bc_Reader *reader, uint64_t *value)
{
    uint8_t bytes[8] = {0};
    const Om_Status status = reader_bytes(reader, bytes, sizeof(bytes));
    if (status == OM_STATUS_OK) {
        *value = (uint64_t)bytes[0]
            | ((uint64_t)bytes[1] << 8)
            | ((uint64_t)bytes[2] << 16)
            | ((uint64_t)bytes[3] << 24)
            | ((uint64_t)bytes[4] << 32)
            | ((uint64_t)bytes[5] << 40)
            | ((uint64_t)bytes[6] << 48)
            | ((uint64_t)bytes[7] << 56);
    }
    return status;
}

static Om_Status encode_image(const Om_Bytecode_Image *image,
                              Om_Bc_Writer *writer)
{
    static const uint8_t magic[4] = {'O', 'M', 'B', 'C'};
    Om_Status status = writer_bytes(writer, magic, sizeof(magic));
    if (status == OM_STATUS_OK) {
        status = writer_u16(writer, OM_BYTECODE_FORMAT_VERSION);
    }
    if (status == OM_STATUS_OK) {
        status = writer_u16(writer, 0);
    }
    const size_t counts[] = {
        image->constant_count, image->function_count, image->instruction_count,
        image->source_map_count, image->actor_count, image->handler_count,
        image->schema_count, image->schema_field_count
    };
    for (size_t index = 0; index < 8 && status == OM_STATUS_OK; ++index) {
        if (counts[index] > UINT32_MAX) {
            return OM_STATUS_OVERFLOW;
        }
        status = writer_u32(writer, (uint32_t)counts[index]);
    }
    if (status == OM_STATUS_OK) {
        status = writer_u64(writer, image->program_fingerprint);
    }
    if (status == OM_STATUS_OK) {
        status = writer_u64(writer, image->interface_fingerprint);
    }
    for (size_t index = 0; index < image->constant_count
         && status == OM_STATUS_OK; ++index) {
        const Om_Bytecode_Constant *constant = &image->constants[index];
        status = writer_u8(writer, (uint8_t)constant->kind);
        if (status != OM_STATUS_OK) {
            break;
        }
        switch (constant->kind) {
        case OM_BYTECODE_CONSTANT_NULL:
        case OM_BYTECODE_CONSTANT_UNIT:
            break;
        case OM_BYTECODE_CONSTANT_BOOL:
            status = writer_u8(writer, constant->as.boolean_value ? 1 : 0);
            break;
        case OM_BYTECODE_CONSTANT_INT: {
            uint64_t bits = 0;
            (void)memcpy(&bits, &constant->as.integer_value, sizeof(bits));
            status = writer_u64(writer, bits);
            break;
        }
        case OM_BYTECODE_CONSTANT_FLOAT: {
            uint64_t bits = 0;
            if (sizeof(constant->as.float_value) != sizeof(bits)) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            (void)memcpy(&bits, &constant->as.float_value, sizeof(bits));
            status = writer_u64(writer, bits);
            break;
        }
        case OM_BYTECODE_CONSTANT_DOWN_REASON:
            status = writer_u32(writer, constant->as.down_reason_value);
            break;
        case OM_BYTECODE_CONSTANT_STRING:
            if (constant->as.string_value.length > UINT32_MAX) {
                return OM_STATUS_OVERFLOW;
            }
            status = writer_u32(
                writer, (uint32_t)constant->as.string_value.length);
            if (status == OM_STATUS_OK) {
                status = writer_bytes(writer, constant->as.string_value.data,
                                      constant->as.string_value.length);
            }
            break;
        }
    }
    for (size_t index = 0; index < image->function_count
         && status == OM_STATUS_OK; ++index) {
        const Om_Bytecode_Function *function = &image->functions[index];
        const uint32_t fields[] = {
            function->name_constant, function->entry_pc, function->end_pc,
            function->parameter_count, function->local_count,
            function->max_stack, function->flags
        };
        for (size_t field = 0; field < 7 && status == OM_STATUS_OK; ++field) {
            status = writer_u32(writer, fields[field]);
        }
    }
    for (size_t index = 0; index < image->instruction_count
         && status == OM_STATUS_OK; ++index) {
        const Om_Bytecode_Instruction *instruction =
            &image->instructions[index];
        uint64_t immediate_bits = 0;
        (void)memcpy(&immediate_bits, &instruction->immediate,
                     sizeof(immediate_bits));
        status = writer_u8(writer, (uint8_t)instruction->opcode);
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, instruction->operand_a);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, instruction->operand_b);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u64(writer, immediate_bits);
        }
    }
    for (size_t index = 0; index < image->source_map_count
         && status == OM_STATUS_OK; ++index) {
        const Om_Bytecode_Source_Map_Entry *entry = &image->source_map[index];
        status = writer_u32(writer, entry->start_pc);
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, entry->end_pc);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, entry->span.source.value);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, entry->span.start);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, entry->span.end);
        }
    }
    for (size_t index = 0; index < image->actor_count
         && status == OM_STATUS_OK; ++index) {
        const Om_Bytecode_Actor *actor = &image->actors[index];
        const uint32_t fields[] = {
            actor->actor_symbol.value, actor->initializer_function,
            actor->state_count, actor->first_handler, actor->handler_count
        };
        for (size_t field = 0; field < 5 && status == OM_STATUS_OK; ++field) {
            status = writer_u32(writer, fields[field]);
        }
    }
    for (size_t index = 0; index < image->handler_count
         && status == OM_STATUS_OK; ++index) {
        status = writer_u32(writer, image->handlers[index].message_symbol.value);
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, image->handlers[index].function_index);
        }
    }
    for (size_t index = 0; index < image->schema_count
         && status == OM_STATUS_OK; ++index) {
        const Om_Bytecode_Schema *schema = &image->schemas[index];
        status = writer_u32(writer, schema->symbol_id.value);
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, (uint32_t)schema->kind);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, schema->first_field);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u32(writer, schema->field_count);
        }
        if (status == OM_STATUS_OK) {
            status = writer_u64(writer, schema->fingerprint);
        }
    }
    for (size_t index = 0; index < image->schema_field_count
         && status == OM_STATUS_OK; ++index) {
        status = writer_u32(writer, image->schema_fields[index].name_constant);
    }
    return status;
}

Om_Status om_bytecode_encode(const Om_Bytecode_Image *image,
                             Om_Byte_Slice *encoded)
{
    if (image == NULL || encoded == NULL || encoded->data != NULL
        || !host_supports_binary64()) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bytecode_Verify_Result verify = {0};
    Om_Status status = om_bytecode_verify(image, &verify);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (!verify.valid) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bc_Writer writer = {0};
    status = encode_image(image, &writer);
    if (status != OM_STATUS_OK) {
        om_free(writer.data);
        return status;
    }
    encoded->data = writer.data;
    encoded->length = writer.length;
    return OM_STATUS_OK;
}

static Om_Status decode_constant(Om_Bc_Reader *reader,
                                 Om_Bytecode_Storage *storage)
{
    uint8_t tag = 0;
    Om_Status status = reader_u8(reader, &tag);
    if (status != OM_STATUS_OK || tag > OM_BYTECODE_CONSTANT_STRING) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bytecode_Constant constant = {.kind = (Om_Bytecode_Constant_Kind)tag};
    uint64_t bits = 0;
    switch (constant.kind) {
    case OM_BYTECODE_CONSTANT_NULL:
    case OM_BYTECODE_CONSTANT_UNIT:
        break;
    case OM_BYTECODE_CONSTANT_BOOL: {
        uint8_t value = 0;
        status = reader_u8(reader, &value);
        if (status != OM_STATUS_OK || value > 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        constant.as.boolean_value = value != 0;
        break;
    }
    case OM_BYTECODE_CONSTANT_INT:
        status = reader_u64(reader, &bits);
        if (status == OM_STATUS_OK) {
            (void)memcpy(&constant.as.integer_value, &bits, sizeof(bits));
        }
        break;
    case OM_BYTECODE_CONSTANT_FLOAT:
        status = reader_u64(reader, &bits);
        if (status == OM_STATUS_OK
            && sizeof(constant.as.float_value) == sizeof(bits)) {
            (void)memcpy(&constant.as.float_value, &bits, sizeof(bits));
        } else if (status == OM_STATUS_OK) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        break;
    case OM_BYTECODE_CONSTANT_DOWN_REASON:
        status = reader_u32(reader, &constant.as.down_reason_value);
        break;
    case OM_BYTECODE_CONSTANT_STRING: {
        uint32_t length = 0;
        status = reader_u32(reader, &length);
        if (status != OM_STATUS_OK
            || (size_t)length > reader->length - reader->offset) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        char *string = NULL;
        if (length > 0) {
            string = om_alloc(length);
            if (string == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            status = reader_bytes(reader, string, length);
            if (status != OM_STATUS_OK) {
                om_free(string);
                return status;
            }
        }
        constant.as.string_value =
            (Om_String_View){.data = string, .length = length};
        break;
    }
    }
    if (status != OM_STATUS_OK) {
        if (constant.kind == OM_BYTECODE_CONSTANT_STRING) {
            om_free((void *)constant.as.string_value.data);
        }
        return status;
    }
    status = om_vector_push(&storage->constants, &constant);
    if (status != OM_STATUS_OK
        && constant.kind == OM_BYTECODE_CONSTANT_STRING) {
        om_free((void *)constant.as.string_value.data);
    }
    return status;
}

static Om_Status require_minimum_records(Om_Bc_Reader *reader,
                                         const uint32_t counts[8])
{
    const size_t record_sizes[8] = {1, 28, 17, 20, 20, 8, 24, 4};
    size_t minimum = 0;
    for (size_t index = 0; index < 8; ++index) {
        size_t group_size = 0;
        if (!checked_mul_size(counts[index], record_sizes[index], &group_size)
            || !checked_add_size(minimum, group_size, &minimum)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    return minimum <= reader->length - reader->offset
        ? OM_STATUS_OK : OM_STATUS_INVALID_ARGUMENT;
}

Om_Status om_bytecode_decode(
    Om_Const_Byte_Slice encoded,
    size_t maximum_byte_length,
    Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *verify_result
)
{
    static const uint8_t expected_magic[4] = {'O', 'M', 'B', 'C'};
    if ((encoded.data == NULL && encoded.length != 0)
        || image == NULL || image->owned_storage != NULL
        || !host_supports_binary64()) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t maximum = maximum_byte_length == 0
        ? OM_BYTECODE_DEFAULT_MAX_BYTES : maximum_byte_length;
    if (encoded.length > maximum || encoded.length < 56) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bc_Reader reader = {
        .data = encoded.data, .length = encoded.length, .offset = 0
    };
    uint8_t magic[4] = {0};
    uint16_t version = 0;
    uint16_t reserved = 0;
    uint32_t counts[8] = {0};
    Om_Status status = reader_bytes(&reader, magic, sizeof(magic));
    if (status == OM_STATUS_OK) {
        status = reader_u16(&reader, &version);
    }
    if (status == OM_STATUS_OK) {
        status = reader_u16(&reader, &reserved);
    }
    for (size_t index = 0; index < 8 && status == OM_STATUS_OK; ++index) {
        status = reader_u32(&reader, &counts[index]);
    }
    uint64_t program_fingerprint = 0;
    uint64_t interface_fingerprint = 0;
    if (status == OM_STATUS_OK) {
        status = reader_u64(&reader, &program_fingerprint);
    }
    if (status == OM_STATUS_OK) {
        status = reader_u64(&reader, &interface_fingerprint);
    }
    if (status != OM_STATUS_OK
        || memcmp(magic, expected_magic, sizeof(magic)) != 0
        || version != OM_BYTECODE_FORMAT_VERSION || reserved != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = require_minimum_records(&reader, counts);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Bytecode_Storage *storage = NULL;
    status = om_bytecode_storage_create(&storage);
    if (status != OM_STATUS_OK) {
        return status;
    }
    for (uint32_t index = 0; index < counts[0] && status == OM_STATUS_OK;
         ++index) {
        status = decode_constant(&reader, storage);
    }
    for (uint32_t index = 0; index < counts[1] && status == OM_STATUS_OK;
         ++index) {
        Om_Bytecode_Function function = {0};
        uint32_t *fields[] = {
            &function.name_constant, &function.entry_pc, &function.end_pc,
            &function.parameter_count, &function.local_count,
            &function.max_stack, &function.flags
        };
        for (size_t field = 0; field < 7 && status == OM_STATUS_OK; ++field) {
            status = reader_u32(&reader, fields[field]);
        }
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&storage->functions, &function);
        }
    }
    for (uint32_t index = 0; index < counts[2] && status == OM_STATUS_OK;
         ++index) {
        Om_Bytecode_Instruction instruction = {0};
        uint8_t opcode = 0;
        uint64_t immediate_bits = 0;
        status = reader_u8(&reader, &opcode);
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &instruction.operand_a);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &instruction.operand_b);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u64(&reader, &immediate_bits);
        }
        if (status == OM_STATUS_OK) {
            instruction.opcode = (Om_Bytecode_Opcode)opcode;
            (void)memcpy(&instruction.immediate, &immediate_bits,
                         sizeof(immediate_bits));
            status = om_vector_push(&storage->instructions, &instruction);
        }
    }
    for (uint32_t index = 0; index < counts[3] && status == OM_STATUS_OK;
         ++index) {
        Om_Bytecode_Source_Map_Entry entry = {0};
        status = reader_u32(&reader, &entry.start_pc);
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &entry.end_pc);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &entry.span.source.value);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &entry.span.start);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &entry.span.end);
        }
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&storage->source_map, &entry);
        }
    }
    for (uint32_t index = 0; index < counts[4] && status == OM_STATUS_OK;
         ++index) {
        Om_Bytecode_Actor actor = {0};
        status = reader_u32(&reader, &actor.actor_symbol.value);
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &actor.initializer_function);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &actor.state_count);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &actor.first_handler);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &actor.handler_count);
        }
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&storage->actors, &actor);
        }
    }
    for (uint32_t index = 0; index < counts[5] && status == OM_STATUS_OK;
         ++index) {
        Om_Bytecode_Actor_Handler handler = {0};
        status = reader_u32(&reader, &handler.message_symbol.value);
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &handler.function_index);
        }
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&storage->handlers, &handler);
        }
    }
    for (uint32_t index = 0; index < counts[6] && status == OM_STATUS_OK;
         ++index) {
        Om_Bytecode_Schema schema = {0};
        uint32_t kind = 0;
        status = reader_u32(&reader, &schema.symbol_id.value);
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &kind);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &schema.first_field);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u32(&reader, &schema.field_count);
        }
        if (status == OM_STATUS_OK) {
            status = reader_u64(&reader, &schema.fingerprint);
        }
        if (status == OM_STATUS_OK) {
            schema.kind = (Om_Bytecode_Schema_Kind)kind;
            status = om_vector_push(&storage->schemas, &schema);
        }
    }
    for (uint32_t index = 0; index < counts[7] && status == OM_STATUS_OK;
         ++index) {
        Om_Bytecode_Schema_Field field = {0};
        status = reader_u32(&reader, &field.name_constant);
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&storage->schema_fields, &field);
        }
    }
    if (status == OM_STATUS_OK && reader.offset != reader.length) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status != OM_STATUS_OK) {
        om_bytecode_storage_destroy(storage);
        return status;
    }
    image->owned_storage = storage;
    image->program_fingerprint = program_fingerprint;
    image->interface_fingerprint = interface_fingerprint;
    om_bytecode_storage_bind_image(storage, image);
    Om_Bytecode_Verify_Result local_verify = {0};
    status = om_bytecode_verify(image,
        verify_result != NULL ? verify_result : &local_verify);
    const Om_Bytecode_Verify_Result *verified =
        verify_result != NULL ? verify_result : &local_verify;
    if (status != OM_STATUS_OK || !verified->valid) {
        om_bytecode_image_destroy(image);
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    return OM_STATUS_OK;
}

void om_byte_slice_destroy(Om_Byte_Slice *slice)
{
    if (slice == NULL) {
        return;
    }
    om_free(slice->data);
    *slice = (Om_Byte_Slice){0};
}

Om_Status om_bytecode_source_map_lookup(
    const Om_Bytecode_Image *image,
    uint32_t instruction_index,
    Om_Source_Span *span
)
{
    if (image == NULL || span == NULL
        || (image->source_map_count > 0 && image->source_map == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t low = 0;
    size_t high = image->source_map_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const Om_Bytecode_Source_Map_Entry *entry = &image->source_map[middle];
        if (instruction_index < entry->start_pc) {
            high = middle;
        } else if (instruction_index >= entry->end_pc) {
            low = middle + 1;
        } else {
            *span = entry->span;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}
