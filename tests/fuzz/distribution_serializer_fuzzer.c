#include <stddef.h>
#include <stdint.h>

#include "ombasic/distribution/serializer.h"
#include "ombasic/foundation/memory.h"

static uint8_t byte_at(const uint8_t *data, size_t size, size_t index)
{
    return index < size ? data[index] : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    enum { MAX_FIELDS = 16 };
    const size_t maximum_input_size = 64 * 1024;
    if (size > maximum_input_size) return 0;

    static const Om_Distribution_Value_Kind kinds[] = {
        OM_DISTRIBUTION_VALUE_NULL,
        OM_DISTRIBUTION_VALUE_UNIT,
        OM_DISTRIBUTION_VALUE_BOOL,
        OM_DISTRIBUTION_VALUE_INT64,
        OM_DISTRIBUTION_VALUE_FLOAT64,
        OM_DISTRIBUTION_VALUE_STRING,
        OM_DISTRIBUTION_VALUE_LIST,
        OM_DISTRIBUTION_VALUE_MAP,
        OM_DISTRIBUTION_VALUE_RECORD,
        OM_DISTRIBUTION_VALUE_MESSAGE,
        OM_DISTRIBUTION_VALUE_ACTOR_REF,
        OM_DISTRIBUTION_VALUE_ACTOR_ID,
        OM_DISTRIBUTION_VALUE_DOWN_REASON,
    };
    Om_Distribution_Value fields[MAX_FIELDS] = {0};
    Om_Distribution_Value nested_values[MAX_FIELDS] = {0};
    Om_Distribution_Map_Entry map_entries[MAX_FIELDS] = {0};
    const size_t field_count = size == 0 ? 0 : byte_at(data, size, 0) % MAX_FIELDS;
    size_t cursor = 1;
    for (size_t index = 0; index < field_count; ++index) {
        Om_Distribution_Value *value = &fields[index];
        value->kind = kinds[byte_at(data, size, cursor++)
            % (sizeof(kinds) / sizeof(kinds[0]))];
        switch (value->kind) {
        case OM_DISTRIBUTION_VALUE_BOOL:
            value->as.boolean_value = (byte_at(data, size, cursor++) & 1u) != 0;
            break;
        case OM_DISTRIBUTION_VALUE_INT64:
            value->as.integer_value = (int64_t)byte_at(data, size, cursor++);
            break;
        case OM_DISTRIBUTION_VALUE_ACTOR_ID:
            value->as.actor_id = byte_at(data, size, cursor++);
            break;
        case OM_DISTRIBUTION_VALUE_ACTOR_REF:
            for (size_t byte = 0;
                 byte < sizeof(value->as.actor_ref.node.bytes); ++byte) {
                value->as.actor_ref.node.bytes[byte] =
                    byte_at(data, size, cursor++);
            }
            value->as.actor_ref.actor_id =
                byte_at(data, size, cursor++);
            break;
        case OM_DISTRIBUTION_VALUE_FLOAT64:
            value->as.float_value = (double)(int8_t)byte_at(data, size, cursor++);
            break;
        case OM_DISTRIBUTION_VALUE_STRING: {
            const size_t start = cursor < size ? cursor : size;
            const size_t length = size - start < 32 ? size - start : 32;
            value->as.string_value = (Om_String_View){
                (const char *)(data + start), length,
            };
            cursor = start + length;
            break;
        }
        case OM_DISTRIBUTION_VALUE_DOWN_REASON:
            value->as.down_reason = byte_at(data, size, cursor++);
            break;
        case OM_DISTRIBUTION_VALUE_LIST:
            nested_values[index] = (Om_Distribution_Value){
                .kind = OM_DISTRIBUTION_VALUE_INT64,
                .as.integer_value = (int64_t)byte_at(data, size, cursor++),
            };
            value->as.list.items = &nested_values[index];
            value->as.list.count = 1;
            break;
        case OM_DISTRIBUTION_VALUE_MAP:
            map_entries[index].key = (Om_Distribution_Value){
                .kind = OM_DISTRIBUTION_VALUE_INT64,
                .as.integer_value = (int64_t)byte_at(data, size, cursor++),
            };
            map_entries[index].value = (Om_Distribution_Value){
                .kind = OM_DISTRIBUTION_VALUE_BOOL,
                .as.boolean_value =
                    (byte_at(data, size, cursor++) & 1u) != 0,
            };
            value->as.map.entries = &map_entries[index];
            value->as.map.count = 1;
            break;
        case OM_DISTRIBUTION_VALUE_RECORD:
        case OM_DISTRIBUTION_VALUE_MESSAGE:
            nested_values[index] = (Om_Distribution_Value){
                .kind = OM_DISTRIBUTION_VALUE_INT64,
                .as.integer_value = (int64_t)byte_at(data, size, cursor++),
            };
            value->as.composite.schema_fingerprint =
                UINT64_C(0x4f4d424653455249);
            value->as.composite.fields = &nested_values[index];
            value->as.composite.field_count = 1;
            break;
        case OM_DISTRIBUTION_VALUE_NULL:
        case OM_DISTRIBUTION_VALUE_UNIT:
            break;
        }
    }

    const Om_Distribution_Serializer_Limits limits = {
        .maximum_payload_bytes = maximum_input_size,
        .maximum_collection_length = MAX_FIELDS,
        .maximum_nesting_depth = 8,
        .maximum_total_values = MAX_FIELDS * 3,
    };
    Om_Byte_Slice encoded = {0};
    if (om_distribution_message_serialize(fields, field_count, limits,
            &encoded) == OM_STATUS_OK) {
        Om_Distribution_Message decoded = {0};
        (void)om_distribution_message_deserialize(
            (Om_Const_Byte_Slice){encoded.data, encoded.length},
            UINT64_C(0x4f4d424653455249), UINT64_C(0x4f4d424653455249),
            field_count, limits, &decoded);
        om_distribution_message_destroy(&decoded);
    }
    om_free(encoded.data);
    return 0;
}
