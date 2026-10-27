#include "ombasic/distribution/transport.h"

static Om_Status exchange_handshake_frame(
    Om_Distribution_Connection *connection,
    const Om_Distribution_Node_Handshake *local,
    Om_Distribution_Frame_Type frame_type,
    size_t maximum_frame_bytes,
    Om_Distribution_Node_Handshake *remote)
{
    uint8_t payload[16] = {0};
    Om_Distribution_Frame outgoing = {0};
    Om_Status status = om_distribution_handshake_frame_init(
        local, frame_type, payload, &outgoing);
    if (status == OM_STATUS_OK) {
        status = om_distribution_connection_send(
            connection, &outgoing, maximum_frame_bytes);
    }
    Om_Distribution_Frame incoming = {0};
    if (status == OM_STATUS_OK) {
        status = om_distribution_connection_receive(
            connection, maximum_frame_bytes, &incoming);
    }
    if (status == OM_STATUS_OK && incoming.type != frame_type) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        status = om_distribution_handshake_frame_read(&incoming, remote);
    }
    return status;
}

Om_Status om_distribution_connection_handshake(
    Om_Distribution_Connection *connection,
    const Om_Distribution_Node_Handshake *local,
    const Om_Distribution_Node_Id *expected_peer,
    size_t maximum_frame_bytes)
{
    return om_distribution_connection_handshake_peer(
        connection, local, expected_peer, maximum_frame_bytes, NULL);
}

Om_Status om_distribution_connection_handshake_peer(
    Om_Distribution_Connection *connection,
    const Om_Distribution_Node_Handshake *local,
    const Om_Distribution_Node_Id *expected_peer,
    size_t maximum_frame_bytes,
    Om_Distribution_Node_Handshake *remote_out)
{
    if (connection == NULL || connection->owned_state == NULL
        || local == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Node_Handshake remote = {0};
    Om_Status status = exchange_handshake_frame(
        connection, local, OM_DISTRIBUTION_FRAME_HELLO,
        maximum_frame_bytes, &remote);
    if (status == OM_STATUS_OK) {
        status = om_distribution_handshake_validate(
            local, &remote, expected_peer);
    }
    if (status == OM_STATUS_OK) {
        Om_Distribution_Node_Handshake acknowledgement = {0};
        status = exchange_handshake_frame(
            connection, local, OM_DISTRIBUTION_FRAME_HELLO_ACK,
            maximum_frame_bytes, &acknowledgement);
        if (status == OM_STATUS_OK) {
            status = om_distribution_handshake_validate(
                local, &acknowledgement, &remote.node_id);
        }
    }
    if (status == OM_STATUS_OK && remote_out != NULL) {
        *remote_out = remote;
    }
    if (status != OM_STATUS_OK) {
        om_distribution_connection_close(connection);
    }
    return status;
}
