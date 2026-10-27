#ifndef OMBASIC_DISTRIBUTION_PROTOCOL_H
#define OMBASIC_DISTRIBUTION_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"

#define OM_DISTRIBUTION_PROTOCOL_VERSION UINT16_C(3)
#define OM_DISTRIBUTION_FRAME_HEADER_SIZE ((size_t)68)
#define OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES ((size_t)16 * 1024 * 1024)

typedef struct Om_Distribution_Node_Id {
    uint8_t bytes[16];
} Om_Distribution_Node_Id;

/* Generates a fresh 128-bit node identity from the operating system RNG. */
Om_Status om_distribution_node_id_generate(Om_Distribution_Node_Id *node_id);

typedef struct Om_Distribution_Actor_Ref {
    Om_Distribution_Node_Id node;
    uint64_t actor_id;
} Om_Distribution_Actor_Ref;

typedef enum Om_Distribution_Frame_Type {
    OM_DISTRIBUTION_FRAME_HELLO = 1,
    OM_DISTRIBUTION_FRAME_HELLO_ACK,
    OM_DISTRIBUTION_FRAME_MESSAGE,
    OM_DISTRIBUTION_FRAME_WATCH,
    OM_DISTRIBUTION_FRAME_WATCH_ACK,
    OM_DISTRIBUTION_FRAME_ACTOR_DOWN,
    OM_DISTRIBUTION_FRAME_SPAWN,
    OM_DISTRIBUTION_FRAME_SPAWN_ACK
} Om_Distribution_Frame_Type;

typedef struct Om_Distribution_Frame {
    uint16_t protocol_version;
    Om_Distribution_Frame_Type type;
    Om_Distribution_Actor_Ref sender;
    Om_Distribution_Actor_Ref receiver;
    uint64_t message_fingerprint;
    Om_Const_Byte_Slice payload;
} Om_Distribution_Frame;

/* Writes one big-endian frame. The maximum applies to the complete frame. */
Om_Status om_distribution_frame_encode(
    const Om_Distribution_Frame *frame,
    Om_Byte_Slice destination,
    size_t maximum_frame_bytes,
    size_t *written
);

/* Decodes one frame and borrows payload bytes from `encoded`. */
Om_Status om_distribution_frame_decode(
    Om_Const_Byte_Slice encoded,
    size_t maximum_frame_bytes,
    Om_Distribution_Frame *frame,
    size_t *consumed
);

/* Validates a complete fixed-size header without reading or allocating payload. */
Om_Status om_distribution_frame_size_from_header(
    Om_Const_Byte_Slice header,
    size_t maximum_frame_bytes,
    size_t *frame_size
);

bool om_distribution_node_id_equal(
    Om_Distribution_Node_Id left,
    Om_Distribution_Node_Id right
);
bool om_distribution_actor_ref_equal(
    Om_Distribution_Actor_Ref left,
    Om_Distribution_Actor_Ref right
);

/* Stable FNV-1a 64-bit fingerprint over an unambiguous schema encoding. */
Om_Status om_distribution_message_fingerprint(
    Om_String_View canonical_module_path,
    Om_String_View message_name,
    const uint64_t *field_type_ids,
    size_t field_count,
    uint64_t *fingerprint
);

#endif
