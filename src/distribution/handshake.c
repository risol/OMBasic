#include "ombasic/distribution/handshake.h"

#include <stdbool.h>
#include <string.h>

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

static bool node_id_is_zero(Om_Distribution_Node_Id node_id)
{
    uint8_t aggregate = 0;
    for (size_t index = 0; index < sizeof(node_id.bytes); ++index) {
        aggregate |= node_id.bytes[index];
    }
    return aggregate == 0;
}

static bool frame_type_is_handshake(Om_Distribution_Frame_Type frame_type)
{
    return frame_type == OM_DISTRIBUTION_FRAME_HELLO
        || frame_type == OM_DISTRIBUTION_FRAME_HELLO_ACK;
}

Om_Status om_distribution_handshake_frame_init(
    const Om_Distribution_Node_Handshake *handshake,
    Om_Distribution_Frame_Type frame_type,
    uint8_t payload_storage[16],
    Om_Distribution_Frame *frame)
{
    if (handshake == NULL || payload_storage == NULL || frame == NULL
        || handshake->protocol_version != OM_DISTRIBUTION_PROTOCOL_VERSION
        || node_id_is_zero(handshake->node_id)
        || !frame_type_is_handshake(frame_type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    write_u64_be(payload_storage, handshake->program_fingerprint);
    write_u64_be(payload_storage + 8, handshake->interface_fingerprint);
    *frame = (Om_Distribution_Frame){
        .protocol_version = handshake->protocol_version,
        .type = frame_type,
        .sender = {.node = handshake->node_id},
        .payload = {payload_storage, 16}
    };
    return OM_STATUS_OK;
}

Om_Status om_distribution_handshake_frame_read(
    const Om_Distribution_Frame *frame,
    Om_Distribution_Node_Handshake *handshake)
{
    if (handshake == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *handshake = (Om_Distribution_Node_Handshake){0};
    if (frame == NULL || !frame_type_is_handshake(frame->type)
        || frame->protocol_version != OM_DISTRIBUTION_PROTOCOL_VERSION
        || frame->payload.data == NULL || frame->payload.length != 16
        || frame->sender.actor_id != 0 || frame->receiver.actor_id != 0
        || !node_id_is_zero(frame->receiver.node)
        || node_id_is_zero(frame->sender.node)
        || frame->message_fingerprint != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *handshake = (Om_Distribution_Node_Handshake){
        .protocol_version = frame->protocol_version,
        .node_id = frame->sender.node,
        .program_fingerprint = read_u64_be(frame->payload.data),
        .interface_fingerprint = read_u64_be(frame->payload.data + 8)
    };
    return OM_STATUS_OK;
}

Om_Status om_distribution_handshake_validate(
    const Om_Distribution_Node_Handshake *local,
    const Om_Distribution_Node_Handshake *remote,
    const Om_Distribution_Node_Id *expected_peer)
{
    if (local == NULL || remote == NULL
        || local->protocol_version != OM_DISTRIBUTION_PROTOCOL_VERSION
        || remote->protocol_version != OM_DISTRIBUTION_PROTOCOL_VERSION
        || node_id_is_zero(local->node_id) || node_id_is_zero(remote->node_id)
        || om_distribution_node_id_equal(local->node_id, remote->node_id)
        || (expected_peer != NULL
            && !om_distribution_node_id_equal(*expected_peer, remote->node_id))
        || local->program_fingerprint != remote->program_fingerprint
        || local->interface_fingerprint != remote->interface_fingerprint) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_OK;
}
