#ifndef OMBASIC_DISTRIBUTION_SERIALIZER_H
#define OMBASIC_DISTRIBUTION_SERIALIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/distribution/protocol.h"

typedef enum Om_Distribution_Value_Kind {
    OM_DISTRIBUTION_VALUE_NULL = 0,
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
    OM_DISTRIBUTION_VALUE_DOWN_REASON
} Om_Distribution_Value_Kind;

typedef struct Om_Distribution_Value Om_Distribution_Value;
typedef struct Om_Distribution_Map_Entry Om_Distribution_Map_Entry;

struct Om_Distribution_Value {
    Om_Distribution_Value_Kind kind;
    union {
        bool boolean_value;
        int64_t integer_value;
        double float_value;
        Om_String_View string_value;
        struct {
            const Om_Distribution_Value *items;
            size_t count;
        } list;
        struct {
            const Om_Distribution_Map_Entry *entries;
            size_t count;
        } map;
        struct {
            uint64_t schema_fingerprint;
            const Om_Distribution_Value *fields;
            size_t field_count;
        } composite;
        Om_Distribution_Actor_Ref actor_ref;
        uint64_t actor_id;
        uint32_t down_reason;
    } as;
};

struct Om_Distribution_Map_Entry {
    Om_Distribution_Value key;
    Om_Distribution_Value value;
};

typedef struct Om_Distribution_Serializer_Limits {
    size_t maximum_payload_bytes;
    size_t maximum_collection_length;
    size_t maximum_nesting_depth;
    /* Caps values across the entire message to bound nested allocations. */
    size_t maximum_total_values;
} Om_Distribution_Serializer_Limits;

typedef struct Om_Distribution_Message {
    uint64_t schema_fingerprint;
    Om_Distribution_Value *fields;
    size_t field_count;
} Om_Distribution_Message;

#define OM_DISTRIBUTION_DEFAULT_MAX_NESTING_DEPTH ((size_t)64)
#define OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH ((size_t)1000000)
#define OM_DISTRIBUTION_DEFAULT_MAX_TOTAL_VALUES ((size_t)1000000)
#define OM_DISTRIBUTION_DEFAULT_MAX_PAYLOAD_BYTES \
    (OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES \
     - OM_DISTRIBUTION_FRAME_HEADER_SIZE)

/* Produces an owned field payload; the frame header carries the fingerprint. */
Om_Status om_distribution_message_serialize(
    const Om_Distribution_Value *fields,
    size_t field_count,
    Om_Distribution_Serializer_Limits limits,
    Om_Byte_Slice *payload
);

/* Decoded fields own all nested arrays and string bytes until destroyed. */
Om_Status om_distribution_message_deserialize(
    Om_Const_Byte_Slice payload,
    uint64_t received_schema_fingerprint,
    uint64_t expected_schema_fingerprint,
    size_t expected_field_count,
    Om_Distribution_Serializer_Limits limits,
    Om_Distribution_Message *message
);
void om_distribution_message_destroy(Om_Distribution_Message *message);

#endif
