#ifndef OMBASIC_DISTRIBUTION_HANDSHAKE_H
#define OMBASIC_DISTRIBUTION_HANDSHAKE_H

#include <stdint.h>

#include "ombasic/distribution/protocol.h"

typedef struct Om_Distribution_Node_Handshake {
    uint16_t protocol_version;
    Om_Distribution_Node_Id node_id;
    uint64_t program_fingerprint;
    uint64_t interface_fingerprint;
} Om_Distribution_Node_Handshake;

/* The 16-byte payload buffer is borrowed by the returned frame. */
Om_Status om_distribution_handshake_frame_init(
    const Om_Distribution_Node_Handshake *handshake,
    Om_Distribution_Frame_Type frame_type,
    uint8_t payload_storage[16],
    Om_Distribution_Frame *frame
);
Om_Status om_distribution_handshake_frame_read(
    const Om_Distribution_Frame *frame,
    Om_Distribution_Node_Handshake *handshake
);
Om_Status om_distribution_handshake_validate(
    const Om_Distribution_Node_Handshake *local,
    const Om_Distribution_Node_Handshake *remote,
    const Om_Distribution_Node_Id *expected_peer
);

#endif
