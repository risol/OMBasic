#include "ombasic/distribution/protocol.h"

#include <stdbool.h>
#include <string.h>

#include "ombasic/foundation/fingerprint.h"
#include "ombasic/foundation/memory.h"

static const uint8_t frame_magic[4] = {'O', 'M', 'B', 'N'};
static const uint8_t schema_domain[] = "OMBASIC-SCHEMA-V1";

static bool byte_slice_is_valid(Om_Const_Byte_Slice slice)
{
    return slice.length == 0 || slice.data != NULL;
}

static bool string_view_is_valid(Om_String_View view)
{
    return view.length != 0 && view.data != NULL;
}

static bool frame_type_is_valid(uint16_t type)
{
    return type >= OM_DISTRIBUTION_FRAME_HELLO
        && type <= OM_DISTRIBUTION_FRAME_SPAWN_ACK;
}

static void write_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) | (uint16_t)source[1]);
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

static size_t normalized_maximum(size_t maximum_frame_bytes)
{
    return maximum_frame_bytes == 0
        ? OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES : maximum_frame_bytes;
}

Om_Status om_distribution_frame_encode(
    const Om_Distribution_Frame *frame,
    Om_Byte_Slice destination,
    size_t maximum_frame_bytes,
    size_t *written)
{
    if (written == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *written = 0;
    if (frame == NULL || !byte_slice_is_valid(frame->payload)
        || (destination.length != 0 && destination.data == NULL)
        || frame->protocol_version != OM_DISTRIBUTION_PROTOCOL_VERSION
        || !frame_type_is_valid((uint16_t)frame->type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (frame->payload.length > UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    size_t total_size = 0;
    if (!om_checked_size_add(OM_DISTRIBUTION_FRAME_HEADER_SIZE,
                             frame->payload.length, &total_size)) {
        return OM_STATUS_OVERFLOW;
    }
    const size_t maximum = normalized_maximum(maximum_frame_bytes);
    if (maximum < OM_DISTRIBUTION_FRAME_HEADER_SIZE
        || total_size > maximum || destination.length < total_size) {
        return OM_STATUS_OUT_OF_RANGE;
    }

    uint8_t *output = destination.data;
    (void)memcpy(output, frame_magic, sizeof(frame_magic));
    write_u16_be(output + 4, frame->protocol_version);
    write_u16_be(output + 6, (uint16_t)frame->type);
    write_u32_be(output + 8, (uint32_t)frame->payload.length);
    (void)memcpy(output + 12, frame->sender.node.bytes, 16);
    write_u64_be(output + 28, frame->sender.actor_id);
    (void)memcpy(output + 36, frame->receiver.node.bytes, 16);
    write_u64_be(output + 52, frame->receiver.actor_id);
    write_u64_be(output + 60, frame->message_fingerprint);
    if (frame->payload.length != 0) {
        (void)memcpy(output + OM_DISTRIBUTION_FRAME_HEADER_SIZE,
                     frame->payload.data, frame->payload.length);
    }
    *written = total_size;
    return OM_STATUS_OK;
}

Om_Status om_distribution_frame_decode(
    Om_Const_Byte_Slice encoded,
    size_t maximum_frame_bytes,
    Om_Distribution_Frame *frame,
    size_t *consumed)
{
    if (frame == NULL || consumed == NULL
        || !byte_slice_is_valid(encoded)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *frame = (Om_Distribution_Frame){0};
    *consumed = 0;
    const size_t maximum = normalized_maximum(maximum_frame_bytes);
    size_t total_size = 0;
    const Om_Status header_status = om_distribution_frame_size_from_header(
        encoded, maximum, &total_size);
    if (header_status != OM_STATUS_OK) {
        return header_status;
    }
    if (encoded.length < total_size) {
        return OM_STATUS_END_OF_INPUT;
    }
    const uint16_t version = read_u16_be(encoded.data + 4);
    const uint16_t type = read_u16_be(encoded.data + 6);
    const uint32_t payload_length = read_u32_be(encoded.data + 8);
    Om_Distribution_Frame decoded = {
        .protocol_version = version,
        .type = (Om_Distribution_Frame_Type)type,
        .sender = {.actor_id = read_u64_be(encoded.data + 28)},
        .receiver = {.actor_id = read_u64_be(encoded.data + 52)},
        .message_fingerprint = read_u64_be(encoded.data + 60),
        .payload = {
            encoded.data + OM_DISTRIBUTION_FRAME_HEADER_SIZE,
            (size_t)payload_length
        }
    };
    (void)memcpy(decoded.sender.node.bytes, encoded.data + 12, 16);
    (void)memcpy(decoded.receiver.node.bytes, encoded.data + 36, 16);
    *frame = decoded;
    *consumed = total_size;
    return OM_STATUS_OK;
}

Om_Status om_distribution_frame_size_from_header(
    Om_Const_Byte_Slice header,
    size_t maximum_frame_bytes,
    size_t *frame_size)
{
    if (frame_size == NULL || !byte_slice_is_valid(header)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *frame_size = 0;
    if (header.length < OM_DISTRIBUTION_FRAME_HEADER_SIZE) {
        return OM_STATUS_END_OF_INPUT;
    }
    const size_t maximum = normalized_maximum(maximum_frame_bytes);
    if (maximum < OM_DISTRIBUTION_FRAME_HEADER_SIZE) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (memcmp(header.data, frame_magic, sizeof(frame_magic)) != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint16_t version = read_u16_be(header.data + 4);
    const uint16_t type = read_u16_be(header.data + 6);
    const uint32_t payload_length = read_u32_be(header.data + 8);
    if (version != OM_DISTRIBUTION_PROTOCOL_VERSION
        || !frame_type_is_valid(type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if ((size_t)payload_length > maximum - OM_DISTRIBUTION_FRAME_HEADER_SIZE) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *frame_size = OM_DISTRIBUTION_FRAME_HEADER_SIZE
        + (size_t)payload_length;
    return OM_STATUS_OK;
}

bool om_distribution_node_id_equal(
    Om_Distribution_Node_Id left,
    Om_Distribution_Node_Id right)
{
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

bool om_distribution_actor_ref_equal(
    Om_Distribution_Actor_Ref left,
    Om_Distribution_Actor_Ref right)
{
    return left.actor_id == right.actor_id
        && om_distribution_node_id_equal(left.node, right.node);
}

Om_Status om_distribution_message_fingerprint(
    Om_String_View canonical_module_path,
    Om_String_View message_name,
    const uint64_t *field_type_fingerprints,
    size_t field_count,
    uint64_t *fingerprint)
{
    if (!string_view_is_valid(canonical_module_path)
        || !string_view_is_valid(message_name)
        || (field_count != 0 && field_type_fingerprints == NULL)
        || fingerprint == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_fingerprint64_schema(
        (Om_String_View){(const char *)schema_domain,
                         sizeof(schema_domain) - 1},
        canonical_module_path, message_name, field_type_fingerprints,
        field_count, fingerprint);
}
