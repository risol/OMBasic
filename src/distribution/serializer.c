#include "ombasic/distribution/serializer.h"

#include <float.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

typedef struct Om_Distribution_Writer {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t maximum_length;
} Om_Distribution_Writer;

typedef struct Om_Distribution_Reader {
    Om_Const_Byte_Slice payload;
    size_t offset;
} Om_Distribution_Reader;

_Static_assert(sizeof(double) == sizeof(uint64_t),
               "Distribution requires binary64-sized doubles");
_Static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == 53,
               "Distribution requires IEEE-754 binary64 doubles");

static Om_Distribution_Serializer_Limits normalized_limits(
    Om_Distribution_Serializer_Limits limits)
{
    if (limits.maximum_payload_bytes == 0) {
        limits.maximum_payload_bytes =
            OM_DISTRIBUTION_DEFAULT_MAX_PAYLOAD_BYTES;
    }
    if (limits.maximum_collection_length == 0) {
        limits.maximum_collection_length =
            OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH;
    }
    if (limits.maximum_nesting_depth == 0) {
        limits.maximum_nesting_depth =
            OM_DISTRIBUTION_DEFAULT_MAX_NESTING_DEPTH;
    }
    if (limits.maximum_total_values == 0) {
        limits.maximum_total_values = OM_DISTRIBUTION_DEFAULT_MAX_TOTAL_VALUES;
    }
    return limits;
}

static void write_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24)
        | ((uint32_t)source[1] << 16)
        | ((uint32_t)source[2] << 8)
        | (uint32_t)source[3];
}

static void write_u64_be(uint8_t *destination, uint64_t value)
{
    for (size_t index = 0; index < 8; ++index) {
        const size_t shift = (7 - index) * 8;
        destination[index] = (uint8_t)(value >> shift);
    }
}

static uint64_t read_u64_be(const uint8_t *source)
{
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value = (value << 8) | (uint64_t)source[index];
    }
    return value;
}

static Om_Status writer_reserve(Om_Distribution_Writer *writer,
                                size_t additional)
{
    size_t required = 0;
    if (!om_checked_size_add(writer->length, additional, &required)) {
        return OM_STATUS_OVERFLOW;
    }
    if (required > writer->maximum_length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (required <= writer->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = writer->capacity == 0 ? 64 : writer->capacity;
    while (capacity < required) {
        size_t grown = 0;
        if (!om_checked_size_mul(capacity, 2, &grown)
            || grown <= capacity) {
            capacity = required;
            break;
        }
        capacity = grown;
    }
    if (capacity > writer->maximum_length) {
        capacity = writer->maximum_length;
    }
    uint8_t *allocation = om_realloc_array(
        writer->data, capacity, sizeof(*writer->data));
    if (allocation == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    writer->data = allocation;
    writer->capacity = capacity;
    return OM_STATUS_OK;
}

static Om_Status writer_bytes(Om_Distribution_Writer *writer,
                              const void *bytes,
                              size_t length)
{
    if (length != 0 && bytes == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Status status = writer_reserve(writer, length);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (length != 0) {
        (void)memcpy(writer->data + writer->length, bytes, length);
    }
    writer->length += length;
    return OM_STATUS_OK;
}

static Om_Status writer_u8(Om_Distribution_Writer *writer, uint8_t value)
{
    return writer_bytes(writer, &value, sizeof(value));
}

static Om_Status writer_u32(Om_Distribution_Writer *writer, uint32_t value)
{
    uint8_t bytes[4] = {0};
    write_u32_be(bytes, value);
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static Om_Status writer_u64(Om_Distribution_Writer *writer, uint64_t value)
{
    uint8_t bytes[8] = {0};
    write_u64_be(bytes, value);
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static Om_Status writer_count(Om_Distribution_Writer *writer,
                              size_t count,
                              Om_Distribution_Serializer_Limits limits)
{
    if (count > limits.maximum_collection_length || count > UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    return writer_u32(writer, (uint32_t)count);
}

static Om_Status write_value(Om_Distribution_Writer *writer,
                             const Om_Distribution_Value *value,
                             size_t depth,
                             size_t *remaining_values,
                             Om_Distribution_Serializer_Limits limits)
{
    if (value == NULL || depth > limits.maximum_nesting_depth) {
        return value == NULL ? OM_STATUS_INVALID_ARGUMENT
                             : OM_STATUS_OUT_OF_RANGE;
    }
    if (remaining_values == NULL || *remaining_values == 0) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *remaining_values -= 1;
    if (value->kind < OM_DISTRIBUTION_VALUE_NULL
        || value->kind > OM_DISTRIBUTION_VALUE_DOWN_REASON) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = writer_u8(writer, (uint8_t)value->kind);
    if (status != OM_STATUS_OK) {
        return status;
    }
    switch (value->kind) {
    case OM_DISTRIBUTION_VALUE_NULL:
    case OM_DISTRIBUTION_VALUE_UNIT:
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_BOOL:
        return writer_u8(writer, value->as.boolean_value ? 1 : 0);
    case OM_DISTRIBUTION_VALUE_INT64: {
        uint64_t bits = 0;
        (void)memcpy(&bits, &value->as.integer_value, sizeof(bits));
        return writer_u64(writer, bits);
    }
    case OM_DISTRIBUTION_VALUE_FLOAT64: {
        uint64_t bits = 0;
        (void)memcpy(&bits, &value->as.float_value, sizeof(bits));
        return writer_u64(writer, bits);
    }
    case OM_DISTRIBUTION_VALUE_STRING:
        if (value->as.string_value.length != 0
            && value->as.string_value.data == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (value->as.string_value.length > limits.maximum_payload_bytes
            || value->as.string_value.length > UINT32_MAX) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        status = om_utf8_validate((Om_Const_Byte_Slice){
            (const uint8_t *)value->as.string_value.data,
            value->as.string_value.length
        });
        if (status != OM_STATUS_OK) {
            return status;
        }
        status = writer_u32(writer, (uint32_t)value->as.string_value.length);
        if (status == OM_STATUS_OK) {
            status = writer_bytes(writer, value->as.string_value.data,
                                  value->as.string_value.length);
        }
        return status;
    case OM_DISTRIBUTION_VALUE_LIST:
        if (value->as.list.count != 0 && value->as.list.items == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = writer_count(writer, value->as.list.count, limits);
        for (size_t index = 0;
             status == OM_STATUS_OK && index < value->as.list.count; ++index) {
            status = write_value(writer, &value->as.list.items[index],
                                 depth + 1, remaining_values, limits);
        }
        return status;
    case OM_DISTRIBUTION_VALUE_MAP:
        if (value->as.map.count != 0 && value->as.map.entries == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = writer_count(writer, value->as.map.count, limits);
        for (size_t index = 0;
             status == OM_STATUS_OK && index < value->as.map.count; ++index) {
            status = write_value(writer,
                &value->as.map.entries[index].key, depth + 1,
                remaining_values, limits);
            if (status == OM_STATUS_OK) {
                status = write_value(writer,
                    &value->as.map.entries[index].value, depth + 1,
                    remaining_values, limits);
            }
        }
        return status;
    case OM_DISTRIBUTION_VALUE_RECORD:
    case OM_DISTRIBUTION_VALUE_MESSAGE:
        if (value->as.composite.field_count != 0
            && value->as.composite.fields == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = writer_u64(writer,
                            value->as.composite.schema_fingerprint);
        if (status == OM_STATUS_OK) {
            status = writer_count(writer, value->as.composite.field_count,
                                  limits);
        }
        for (size_t index = 0;
             status == OM_STATUS_OK
                 && index < value->as.composite.field_count; ++index) {
            status = write_value(writer,
                &value->as.composite.fields[index], depth + 1,
                remaining_values, limits);
        }
        return status;
    case OM_DISTRIBUTION_VALUE_ACTOR_REF:
        status = writer_bytes(writer, value->as.actor_ref.node.bytes,
                              sizeof(value->as.actor_ref.node.bytes));
        if (status == OM_STATUS_OK) {
            status = writer_u64(writer, value->as.actor_ref.actor_id);
        }
        return status;
    case OM_DISTRIBUTION_VALUE_ACTOR_ID:
        return writer_u64(writer, value->as.actor_id);
    case OM_DISTRIBUTION_VALUE_DOWN_REASON:
        return writer_u32(writer, value->as.down_reason);
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status reader_bytes(Om_Distribution_Reader *reader,
                              size_t length,
                              const uint8_t **bytes)
{
    if (bytes == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *bytes = NULL;
    if (reader->offset > reader->payload.length
        || length > reader->payload.length - reader->offset) {
        return OM_STATUS_END_OF_INPUT;
    }
    *bytes = reader->payload.data + reader->offset;
    reader->offset += length;
    return OM_STATUS_OK;
}

static Om_Status reader_u8(Om_Distribution_Reader *reader, uint8_t *value)
{
    const uint8_t *bytes = NULL;
    const Om_Status status = reader_bytes(reader, 1, &bytes);
    if (status == OM_STATUS_OK) {
        *value = bytes[0];
    }
    return status;
}

static Om_Status reader_u32(Om_Distribution_Reader *reader, uint32_t *value)
{
    const uint8_t *bytes = NULL;
    const Om_Status status = reader_bytes(reader, 4, &bytes);
    if (status == OM_STATUS_OK) {
        *value = read_u32_be(bytes);
    }
    return status;
}

static Om_Status reader_u64(Om_Distribution_Reader *reader, uint64_t *value)
{
    const uint8_t *bytes = NULL;
    const Om_Status status = reader_bytes(reader, 8, &bytes);
    if (status == OM_STATUS_OK) {
        *value = read_u64_be(bytes);
    }
    return status;
}

static Om_Status reader_count(Om_Distribution_Reader *reader,
                              Om_Distribution_Serializer_Limits limits,
                              size_t minimum_bytes_per_item,
                              size_t *count)
{
    uint32_t encoded_count = 0;
    const Om_Status status = reader_u32(reader, &encoded_count);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const size_t decoded_count = (size_t)encoded_count;
    if (decoded_count > limits.maximum_collection_length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t remaining = reader->payload.length - reader->offset;
    if (minimum_bytes_per_item != 0
        && decoded_count > remaining / minimum_bytes_per_item) {
        return OM_STATUS_END_OF_INPUT;
    }
    *count = decoded_count;
    return OM_STATUS_OK;
}

static void destroy_value(Om_Distribution_Value *value);
static Om_Status read_value(Om_Distribution_Reader *reader,
                            size_t depth,
                            size_t *remaining_values,
                            Om_Distribution_Serializer_Limits limits,
                            Om_Distribution_Value *value);

static void destroy_values(Om_Distribution_Value *values, size_t count)
{
    if (values == NULL) {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        destroy_value(&values[index]);
    }
    om_free(values);
}

static void destroy_value(Om_Distribution_Value *value)
{
    if (value == NULL) {
        return;
    }
    if (value->kind == OM_DISTRIBUTION_VALUE_STRING) {
        om_free((void *)value->as.string_value.data);
    } else if (value->kind == OM_DISTRIBUTION_VALUE_LIST) {
        destroy_values((Om_Distribution_Value *)value->as.list.items,
                       value->as.list.count);
    } else if (value->kind == OM_DISTRIBUTION_VALUE_MAP) {
        Om_Distribution_Map_Entry *entries =
            (Om_Distribution_Map_Entry *)value->as.map.entries;
        if (entries != NULL) {
            for (size_t index = 0; index < value->as.map.count; ++index) {
                destroy_value(&entries[index].key);
                destroy_value(&entries[index].value);
            }
            om_free(entries);
        }
    } else if (value->kind == OM_DISTRIBUTION_VALUE_RECORD
               || value->kind == OM_DISTRIBUTION_VALUE_MESSAGE) {
        destroy_values((Om_Distribution_Value *)value->as.composite.fields,
                       value->as.composite.field_count);
    }
    *value = (Om_Distribution_Value){0};
}

static Om_Status read_values(Om_Distribution_Reader *reader,
                             size_t depth,
                             Om_Distribution_Serializer_Limits limits,
                             size_t count,
                             size_t minimum_bytes_per_item,
                             size_t *remaining_values,
                             Om_Distribution_Value **values)
{
    if (count == 0) {
        *values = NULL;
        return OM_STATUS_OK;
    }
    if (minimum_bytes_per_item != 0
        && count > (reader->payload.length - reader->offset)
            / minimum_bytes_per_item) {
        return OM_STATUS_END_OF_INPUT;
    }
    if (remaining_values == NULL || count > *remaining_values) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *remaining_values -= count;
    Om_Distribution_Value *items = om_alloc_array(count, sizeof(*items));
    if (items == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(items, 0, count * sizeof(*items));
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        status = read_value(reader, depth + 1, remaining_values, limits,
                            &items[index]);
    }
    if (status != OM_STATUS_OK) {
        destroy_values(items, count);
        return status;
    }
    *values = items;
    return OM_STATUS_OK;
}

static Om_Status read_value(Om_Distribution_Reader *reader,
                            size_t depth,
                            size_t *remaining_values,
                            Om_Distribution_Serializer_Limits limits,
                            Om_Distribution_Value *value)
{
    if (depth > limits.maximum_nesting_depth) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    uint8_t kind_byte = 0;
    Om_Status status = reader_u8(reader, &kind_byte);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (kind_byte > OM_DISTRIBUTION_VALUE_DOWN_REASON) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    value->kind = (Om_Distribution_Value_Kind)kind_byte;
    switch (value->kind) {
    case OM_DISTRIBUTION_VALUE_NULL:
    case OM_DISTRIBUTION_VALUE_UNIT:
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_BOOL: {
        uint8_t boolean = 0;
        status = reader_u8(reader, &boolean);
        if (status == OM_STATUS_OK && boolean > 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        value->as.boolean_value = boolean != 0;
        return status;
    }
    case OM_DISTRIBUTION_VALUE_INT64: {
        uint64_t bits = 0;
        status = reader_u64(reader, &bits);
        if (status == OM_STATUS_OK) {
            (void)memcpy(&value->as.integer_value, &bits, sizeof(bits));
        }
        return status;
    }
    case OM_DISTRIBUTION_VALUE_FLOAT64: {
        uint64_t bits = 0;
        status = reader_u64(reader, &bits);
        if (status == OM_STATUS_OK) {
            (void)memcpy(&value->as.float_value, &bits, sizeof(bits));
        }
        return status;
    }
    case OM_DISTRIBUTION_VALUE_STRING: {
        uint32_t length = 0;
        status = reader_u32(reader, &length);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if ((size_t)length > limits.maximum_payload_bytes) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        const uint8_t *bytes = NULL;
        status = reader_bytes(reader, (size_t)length, &bytes);
        if (status != OM_STATUS_OK) {
            return status;
        }
        status = om_utf8_validate((Om_Const_Byte_Slice){bytes, length});
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (length != 0) {
            char *copy = om_alloc((size_t)length);
            if (copy == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            (void)memcpy(copy, bytes, (size_t)length);
            value->as.string_value = (Om_String_View){copy, (size_t)length};
        }
        return OM_STATUS_OK;
    }
    case OM_DISTRIBUTION_VALUE_LIST: {
        size_t count = 0;
        status = reader_count(reader, limits, 1, &count);
        if (status == OM_STATUS_OK) {
            Om_Distribution_Value *items = NULL;
            status = read_values(reader, depth, limits, count, 1,
                                 remaining_values, &items);
            if (status == OM_STATUS_OK) {
                value->as.list.items = items;
                value->as.list.count = count;
            }
        }
        return status;
    }
    case OM_DISTRIBUTION_VALUE_MAP: {
        size_t count = 0;
        status = reader_count(reader, limits, 2, &count);
        if (status != OM_STATUS_OK || count == 0) {
            return status;
        }
        size_t value_count = 0;
        if (!om_checked_size_mul(count, 2, &value_count)) {
            return OM_STATUS_OVERFLOW;
        }
        if (remaining_values == NULL || value_count > *remaining_values) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        *remaining_values -= value_count;
        Om_Distribution_Map_Entry *entries = om_alloc_array(
            count, sizeof(*entries));
        if (entries == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memset(entries, 0, count * sizeof(*entries));
        for (size_t index = 0; status == OM_STATUS_OK && index < count;
             ++index) {
            status = read_value(reader, depth + 1, remaining_values,
                                limits, &entries[index].key);
            if (status == OM_STATUS_OK) {
                status = read_value(reader, depth + 1, remaining_values,
                                    limits, &entries[index].value);
            }
        }
        if (status != OM_STATUS_OK) {
            for (size_t index = 0; index < count; ++index) {
                destroy_value(&entries[index].key);
                destroy_value(&entries[index].value);
            }
            om_free(entries);
            return status;
        }
        value->as.map.entries = entries;
        value->as.map.count = count;
        return OM_STATUS_OK;
    }
    case OM_DISTRIBUTION_VALUE_RECORD:
    case OM_DISTRIBUTION_VALUE_MESSAGE: {
        size_t count = 0;
        status = reader_u64(reader,
                            &value->as.composite.schema_fingerprint);
        if (status == OM_STATUS_OK) {
            status = reader_count(reader, limits, 1, &count);
        }
        if (status == OM_STATUS_OK) {
            Om_Distribution_Value *fields = NULL;
            status = read_values(reader, depth, limits, count, 1,
                                 remaining_values, &fields);
            if (status == OM_STATUS_OK) {
                value->as.composite.fields = fields;
                value->as.composite.field_count = count;
            }
        }
        return status;
    }
    case OM_DISTRIBUTION_VALUE_ACTOR_REF: {
        const uint8_t *node_id = NULL;
        status = reader_bytes(reader, sizeof(value->as.actor_ref.node.bytes),
                              &node_id);
        if (status == OM_STATUS_OK) {
            (void)memcpy(value->as.actor_ref.node.bytes, node_id,
                         sizeof(value->as.actor_ref.node.bytes));
            status = reader_u64(reader, &value->as.actor_ref.actor_id);
        }
        return status;
    }
    case OM_DISTRIBUTION_VALUE_ACTOR_ID:
        return reader_u64(reader, &value->as.actor_id);
    case OM_DISTRIBUTION_VALUE_DOWN_REASON:
        return reader_u32(reader, &value->as.down_reason);
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

Om_Status om_distribution_message_serialize(
    const Om_Distribution_Value *fields,
    size_t field_count,
    Om_Distribution_Serializer_Limits limits,
    Om_Byte_Slice *payload)
{
    if (payload == NULL || (field_count != 0 && fields == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *payload = (Om_Byte_Slice){0};
    limits = normalized_limits(limits);
    Om_Distribution_Writer writer = {
        .maximum_length = limits.maximum_payload_bytes
    };
    size_t remaining_values = limits.maximum_total_values;
    Om_Status status = writer_count(&writer, field_count, limits);
    for (size_t index = 0;
         status == OM_STATUS_OK && index < field_count; ++index) {
        status = write_value(&writer, &fields[index], 1, &remaining_values,
                             limits);
    }
    if (status != OM_STATUS_OK) {
        om_free(writer.data);
        return status;
    }
    *payload = (Om_Byte_Slice){writer.data, writer.length};
    return OM_STATUS_OK;
}

Om_Status om_distribution_message_deserialize(
    Om_Const_Byte_Slice payload,
    uint64_t received_schema_fingerprint,
    uint64_t expected_schema_fingerprint,
    size_t expected_field_count,
    Om_Distribution_Serializer_Limits limits,
    Om_Distribution_Message *message)
{
    if (message == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *message = (Om_Distribution_Message){0};
    if (payload.length != 0 && payload.data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    limits = normalized_limits(limits);
    if (payload.length > limits.maximum_payload_bytes) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (received_schema_fingerprint != expected_schema_fingerprint) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Reader reader = {.payload = payload};
    size_t remaining_values = limits.maximum_total_values;
    size_t field_count = 0;
    Om_Status status = reader_count(&reader, limits, 1, &field_count);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (field_count != expected_field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Value *fields = NULL;
    status = read_values(&reader, 0, limits, field_count, 1,
                         &remaining_values, &fields);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (reader.offset != payload.length) {
        destroy_values(fields, field_count);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *message = (Om_Distribution_Message){
        .schema_fingerprint = received_schema_fingerprint,
        .fields = fields,
        .field_count = field_count
    };
    return OM_STATUS_OK;
}

void om_distribution_message_destroy(Om_Distribution_Message *message)
{
    if (message == NULL) {
        return;
    }
    destroy_values(message->fields, message->field_count);
    *message = (Om_Distribution_Message){0};
}
