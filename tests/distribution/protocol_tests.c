#include "ombasic/distribution/protocol.h"
#include "ombasic/distribution/serializer.h"
#include "ombasic/distribution/transport.h"
#include "ombasic/foundation/memory.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#define DIST_ASSERT(expression)                                                  \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",            \
                          __FILE__, __LINE__, #expression);                      \
            return false;                                                        \
        }                                                                        \
    } while (false)

#define DIST_ASSERT_CLEANUP(expression)                                          \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",            \
                          __FILE__, __LINE__, #expression);                      \
            goto cleanup;                                                        \
        }                                                                        \
    } while (false)

static bool test_message_payload_serializer_roundtrip(void)
{
    static const char utf8_text[] = "value-\xe4\xbd\xa0\xe5\xa5\xbd";
    static const Om_Distribution_Value list_items[] = {
        {.kind = OM_DISTRIBUTION_VALUE_INT64,
         .as.integer_value = INT64_MIN},
        {.kind = OM_DISTRIBUTION_VALUE_STRING,
         .as.string_value = {utf8_text, sizeof(utf8_text) - 1}}
    };
    static const Om_Distribution_Map_Entry map_entries[] = {
        {.key = {.kind = OM_DISTRIBUTION_VALUE_STRING,
                 .as.string_value = {"count", 5}},
         .value = {.kind = OM_DISTRIBUTION_VALUE_INT64,
                   .as.integer_value = 7}},
        {.key = {.kind = OM_DISTRIBUTION_VALUE_BOOL,
                 .as.boolean_value = true},
         .value = {.kind = OM_DISTRIBUTION_VALUE_NULL}}
    };
    static const Om_Distribution_Value record_fields[] = {
        {.kind = OM_DISTRIBUTION_VALUE_FLOAT64,
         .as.float_value = -4.125},
        {.kind = OM_DISTRIBUTION_VALUE_LIST,
         .as.list = {list_items, sizeof(list_items) / sizeof(list_items[0])}}
    };
    static const Om_Distribution_Value nested_message_fields[] = {
        {.kind = OM_DISTRIBUTION_VALUE_UNIT},
        {.kind = OM_DISTRIBUTION_VALUE_MAP,
         .as.map = {map_entries,
                    sizeof(map_entries) / sizeof(map_entries[0])}}
    };
    Om_Distribution_Node_Id node = {0};
    for (size_t index = 0; index < sizeof(node.bytes); ++index) {
        node.bytes[index] = (uint8_t)(index + 1);
    }
    const Om_Distribution_Value fields[] = {
        {.kind = OM_DISTRIBUTION_VALUE_NULL},
        {.kind = OM_DISTRIBUTION_VALUE_UNIT},
        {.kind = OM_DISTRIBUTION_VALUE_BOOL,
         .as.boolean_value = true},
        {.kind = OM_DISTRIBUTION_VALUE_INT64,
         .as.integer_value = INT64_MIN},
        {.kind = OM_DISTRIBUTION_VALUE_FLOAT64,
         .as.float_value = 3.141592653589793},
        {.kind = OM_DISTRIBUTION_VALUE_STRING,
         .as.string_value = {utf8_text, sizeof(utf8_text) - 1}},
        {.kind = OM_DISTRIBUTION_VALUE_LIST,
         .as.list = {list_items, sizeof(list_items) / sizeof(list_items[0])}},
        {.kind = OM_DISTRIBUTION_VALUE_MAP,
         .as.map = {map_entries,
                    sizeof(map_entries) / sizeof(map_entries[0])}},
        {.kind = OM_DISTRIBUTION_VALUE_RECORD,
         .as.composite = {UINT64_C(0x123456789abcdef0), record_fields,
                          sizeof(record_fields) / sizeof(record_fields[0])}},
        {.kind = OM_DISTRIBUTION_VALUE_MESSAGE,
         .as.composite = {UINT64_C(0x0fedcba987654321),
                          nested_message_fields,
                          sizeof(nested_message_fields)
                              / sizeof(nested_message_fields[0])}},
        {.kind = OM_DISTRIBUTION_VALUE_ACTOR_REF,
         .as.actor_ref = {.node = node, .actor_id = UINT64_C(0xabcdef)}},
        {.kind = OM_DISTRIBUTION_VALUE_ACTOR_ID,
         .as.actor_id = UINT64_C(0x123456789abcdef0)},
        {.kind = OM_DISTRIBUTION_VALUE_DOWN_REASON,
         .as.down_reason = UINT32_C(3)}
    };
    Om_Byte_Slice payload = {0};
    Om_Distribution_Message decoded = {0};
    bool passed = false;
    const size_t field_count = sizeof(fields) / sizeof(fields[0]);
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(
        fields, field_count, (Om_Distribution_Serializer_Limits){0},
        &payload) == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(payload.data != NULL && payload.length > 4);
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){payload.data, payload.length},
        UINT64_C(0x8877665544332211), UINT64_C(0x8877665544332211),
        field_count, (Om_Distribution_Serializer_Limits){0}, &decoded)
        == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(decoded.schema_fingerprint
                        == UINT64_C(0x8877665544332211));
    DIST_ASSERT_CLEANUP(decoded.field_count == field_count);
    DIST_ASSERT_CLEANUP(decoded.fields[0].kind == OM_DISTRIBUTION_VALUE_NULL);
    DIST_ASSERT_CLEANUP(decoded.fields[1].kind == OM_DISTRIBUTION_VALUE_UNIT);
    DIST_ASSERT_CLEANUP(decoded.fields[2].as.boolean_value);
    DIST_ASSERT_CLEANUP(decoded.fields[3].as.integer_value == INT64_MIN);
    DIST_ASSERT_CLEANUP(decoded.fields[4].as.float_value
                        == fields[4].as.float_value);
    DIST_ASSERT_CLEANUP(decoded.fields[5].as.string_value.length
                        == sizeof(utf8_text) - 1);
    DIST_ASSERT_CLEANUP(memcmp(decoded.fields[5].as.string_value.data,
                              utf8_text, sizeof(utf8_text) - 1) == 0);
    DIST_ASSERT_CLEANUP(decoded.fields[6].as.list.count == 2);
    DIST_ASSERT_CLEANUP(decoded.fields[6].as.list.items[0].as.integer_value
                        == INT64_MIN);
    DIST_ASSERT_CLEANUP(decoded.fields[7].as.map.count == 2);
    DIST_ASSERT_CLEANUP(decoded.fields[8].as.composite.schema_fingerprint
                        == UINT64_C(0x123456789abcdef0));
    DIST_ASSERT_CLEANUP(decoded.fields[8].as.composite.fields[0]
                            .as.float_value == -4.125);
    DIST_ASSERT_CLEANUP(decoded.fields[9].as.composite.schema_fingerprint
                        == UINT64_C(0x0fedcba987654321));
    DIST_ASSERT_CLEANUP(decoded.fields[10].as.actor_ref.actor_id
                        == UINT64_C(0xabcdef));
    DIST_ASSERT_CLEANUP(om_distribution_node_id_equal(
        decoded.fields[10].as.actor_ref.node, node));
    DIST_ASSERT_CLEANUP(decoded.fields[11].kind
                        == OM_DISTRIBUTION_VALUE_ACTOR_ID);
    DIST_ASSERT_CLEANUP(decoded.fields[11].as.actor_id
                        == UINT64_C(0x123456789abcdef0));
    DIST_ASSERT_CLEANUP(decoded.fields[12].kind
                        == OM_DISTRIBUTION_VALUE_DOWN_REASON);
    DIST_ASSERT_CLEANUP(decoded.fields[12].as.down_reason == UINT32_C(3));
    passed = true;

cleanup:
    om_distribution_message_destroy(&decoded);
    om_free(payload.data);
    return passed;
}

static bool test_large_nested_payload_budget(void)
{
    static char large_text[65536];
    static const uint8_t map_key[] = {'r', 'e', 'c', 'o', 'r', 'd'};
    (void)memset(large_text, 'x', sizeof(large_text));
    Om_Distribution_Value record_fields[] = {
        {.kind = OM_DISTRIBUTION_VALUE_STRING,
         .as.string_value = {large_text, sizeof(large_text)}},
    };
    Om_Distribution_Value list_items[] = {
        {.kind = OM_DISTRIBUTION_VALUE_RECORD,
         .as.composite = {UINT64_C(0x1122334455667788), record_fields, 1}},
    };
    Om_Distribution_Map_Entry map_entries[] = {
        {.key = {.kind = OM_DISTRIBUTION_VALUE_STRING,
                 .as.string_value = {(const char *)map_key,
                                     sizeof(map_key)}},
         .value = {.kind = OM_DISTRIBUTION_VALUE_LIST,
                   .as.list = {list_items, 1}}},
    };
    Om_Distribution_Value fields[] = {
        {.kind = OM_DISTRIBUTION_VALUE_MAP,
         .as.map = {map_entries, 1}},
    };
    Om_Byte_Slice payload = {0};
    Om_Byte_Slice limited_payload = {0};
    Om_Distribution_Message decoded = {0};
    bool passed = false;
    const Om_Distribution_Serializer_Limits limits = {
        .maximum_payload_bytes = 131072,
        .maximum_collection_length = 16,
        .maximum_nesting_depth = 16,
        .maximum_total_values = 16,
    };
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(fields, 1, limits,
        &payload) == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(payload.length > sizeof(large_text));
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(fields, 1,
        (Om_Distribution_Serializer_Limits){
            .maximum_payload_bytes = 4096,
            .maximum_collection_length = 16,
            .maximum_nesting_depth = 16,
            .maximum_total_values = 16,
        }, &limited_payload) == OM_STATUS_OUT_OF_RANGE);
    DIST_ASSERT_CLEANUP(limited_payload.data == NULL);
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){payload.data, payload.length},
        UINT64_C(0x8877665544332211), UINT64_C(0x8877665544332211), 1,
        limits, &decoded) == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(decoded.fields[0].kind == OM_DISTRIBUTION_VALUE_MAP);
    DIST_ASSERT_CLEANUP(decoded.fields[0].as.map.count == 1);
    const Om_Distribution_Value *list =
        &decoded.fields[0].as.map.entries[0].value;
    DIST_ASSERT_CLEANUP(list->kind == OM_DISTRIBUTION_VALUE_LIST
                        && list->as.list.count == 1);
    const Om_Distribution_Value *record = &list->as.list.items[0];
    DIST_ASSERT_CLEANUP(record->kind == OM_DISTRIBUTION_VALUE_RECORD
                        && record->as.composite.field_count == 1);
    const Om_Distribution_Value *text = &record->as.composite.fields[0];
    DIST_ASSERT_CLEANUP(text->kind == OM_DISTRIBUTION_VALUE_STRING
                        && text->as.string_value.length == sizeof(large_text)
                        && memcmp(text->as.string_value.data, large_text,
                                  sizeof(large_text)) == 0);
    passed = true;

cleanup:
    om_distribution_message_destroy(&decoded);
    om_free(payload.data);
    om_free(limited_payload.data);
    return passed;
}

static bool test_message_payload_serializer_rejections(void)
{
    static const uint8_t invalid_utf8_bytes[] = {
        UINT8_C(0xc3), UINT8_C(0x78)
    };
    static const Om_Distribution_Value nested_items[] = {
        {.kind = OM_DISTRIBUTION_VALUE_INT64, .as.integer_value = 1},
        {.kind = OM_DISTRIBUTION_VALUE_INT64, .as.integer_value = 2}
    };
    const Om_Distribution_Value nested = {
        .kind = OM_DISTRIBUTION_VALUE_LIST,
        .as.list = {nested_items,
                    sizeof(nested_items) / sizeof(nested_items[0])}
    };
    const Om_Distribution_Value valid = {
        .kind = OM_DISTRIBUTION_VALUE_BOOL,
        .as.boolean_value = true
    };
    const Om_Distribution_Value invalid_string = {
        .kind = OM_DISTRIBUTION_VALUE_STRING,
        .as.string_value = {(const char *)invalid_utf8_bytes,
                            sizeof(invalid_utf8_bytes)}
    };
    Om_Byte_Slice payload = {0};
    Om_Byte_Slice nested_payload = {0};
    Om_Distribution_Message decoded = {0};
    bool passed = false;
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(
        &valid, 1,
        (Om_Distribution_Serializer_Limits){.maximum_payload_bytes = 4},
        &payload) == OM_STATUS_OUT_OF_RANGE);
    DIST_ASSERT_CLEANUP(payload.data == NULL);
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(
        &invalid_string, 1, (Om_Distribution_Serializer_Limits){0},
        &payload) == OM_STATUS_INVALID_UTF8);
    Om_Distribution_Value too_deep = {
        .kind = OM_DISTRIBUTION_VALUE_LIST,
        .as.list = {.items = &valid, .count = 1}
    };
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(
        &too_deep, 1,
        (Om_Distribution_Serializer_Limits){.maximum_nesting_depth = 1},
        &payload) == OM_STATUS_OUT_OF_RANGE);
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(
        &nested, 1,
        (Om_Distribution_Serializer_Limits){.maximum_total_values = 2},
        &nested_payload) == OM_STATUS_OUT_OF_RANGE);
    DIST_ASSERT_CLEANUP(nested_payload.data == NULL);
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(
        &nested, 1, (Om_Distribution_Serializer_Limits){0},
        &nested_payload) == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){nested_payload.data, nested_payload.length},
        1, 1, 1,
        (Om_Distribution_Serializer_Limits){.maximum_total_values = 2},
        &decoded) == OM_STATUS_OUT_OF_RANGE);
    DIST_ASSERT_CLEANUP(decoded.fields == NULL && decoded.field_count == 0);
    om_free(nested_payload.data);
    nested_payload = (Om_Byte_Slice){0};
    DIST_ASSERT_CLEANUP(om_distribution_message_serialize(
        &valid, 1, (Om_Distribution_Serializer_Limits){0},
        &payload) == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){payload.data, payload.length}, 1, 2, 1,
        (Om_Distribution_Serializer_Limits){0}, &decoded)
        == OM_STATUS_INVALID_ARGUMENT);
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){payload.data, payload.length - 1}, 1, 1, 1,
        (Om_Distribution_Serializer_Limits){0}, &decoded)
        == OM_STATUS_END_OF_INPUT);
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){payload.data, payload.length}, 1, 1, 2,
        (Om_Distribution_Serializer_Limits){0}, &decoded)
        == OM_STATUS_INVALID_ARGUMENT);
    decoded.schema_fingerprint = 9;
    decoded.field_count = 3;
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){NULL, 1}, 1, 1, 1,
        (Om_Distribution_Serializer_Limits){0}, &decoded)
        == OM_STATUS_INVALID_ARGUMENT);
    DIST_ASSERT_CLEANUP(decoded.schema_fingerprint == 0
                        && decoded.fields == NULL
                        && decoded.field_count == 0);
    payload.data[4] = UINT8_C(0xff);
    DIST_ASSERT_CLEANUP(om_distribution_message_deserialize(
        (Om_Const_Byte_Slice){payload.data, payload.length}, 1, 1, 1,
        (Om_Distribution_Serializer_Limits){0}, &decoded)
        == OM_STATUS_INVALID_ARGUMENT);
    passed = true;

cleanup:
    om_distribution_message_destroy(&decoded);
    om_free(payload.data);
    om_free(nested_payload.data);
    return passed;
}

static Om_Distribution_Frame make_frame(const uint8_t *payload,
                                        size_t payload_length)
{
    Om_Distribution_Frame frame = {
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .type = OM_DISTRIBUTION_FRAME_MESSAGE,
        .sender = {.actor_id = UINT64_C(0x0102030405060708)},
        .receiver = {.actor_id = UINT64_C(0x1112131415161718)},
        .message_fingerprint = UINT64_C(0x2122232425262728),
        .payload = {payload, payload_length}
    };
    for (size_t index = 0; index < 16; ++index) {
        frame.sender.node.bytes[index] = (uint8_t)index;
        frame.receiver.node.bytes[index] = (uint8_t)(16 + index);
    }
    return frame;
}

static bool test_frame_roundtrip_and_stream_consumption(void)
{
    static const uint8_t payload[] = {0, 1, 2, 0xfe, 0xff};
    uint8_t encoded[OM_DISTRIBUTION_FRAME_HEADER_SIZE + sizeof(payload) + 3]
        = {0};
    const Om_Distribution_Frame original = make_frame(
        payload, sizeof(payload));
    size_t written = 0;
    DIST_ASSERT(om_distribution_frame_encode(
                   &original, (Om_Byte_Slice){encoded, sizeof(encoded)}, 0,
                   &written) == OM_STATUS_OK);
    DIST_ASSERT(written == OM_DISTRIBUTION_FRAME_HEADER_SIZE + sizeof(payload));
    Om_Distribution_Frame decoded = {0};
    size_t consumed = 0;
    DIST_ASSERT(om_distribution_frame_decode(
                   (Om_Const_Byte_Slice){encoded, written + 3}, 0,
                   &decoded, &consumed) == OM_STATUS_OK);
    DIST_ASSERT(consumed == written);
    DIST_ASSERT(decoded.protocol_version == original.protocol_version);
    DIST_ASSERT(decoded.type == original.type);
    DIST_ASSERT(om_distribution_actor_ref_equal(decoded.sender,
                                                 original.sender));
    DIST_ASSERT(om_distribution_actor_ref_equal(decoded.receiver,
                                                 original.receiver));
    DIST_ASSERT(decoded.message_fingerprint
                == original.message_fingerprint);
    DIST_ASSERT(decoded.payload.length == sizeof(payload));
    DIST_ASSERT(memcmp(decoded.payload.data, payload, sizeof(payload)) == 0);
    DIST_ASSERT(encoded[4]
                    == (uint8_t)(OM_DISTRIBUTION_PROTOCOL_VERSION >> 8)
                && encoded[5]
                    == (uint8_t)OM_DISTRIBUTION_PROTOCOL_VERSION);
    DIST_ASSERT(encoded[28] == 1 && encoded[35] == 8);
    DIST_ASSERT(encoded[60] == 0x21 && encoded[67] == 0x28);
    return true;
}

static bool test_node_identity_generation(void)
{
    Om_Distribution_Node_Id first = {0};
    Om_Distribution_Node_Id second = {0};
    DIST_ASSERT(om_distribution_node_id_generate(&first) == OM_STATUS_OK);
    DIST_ASSERT(om_distribution_node_id_generate(&second) == OM_STATUS_OK);
    DIST_ASSERT(!om_distribution_node_id_equal(first, second));
    DIST_ASSERT((first.bytes[6] & UINT8_C(0xf0)) == UINT8_C(0x40));
    DIST_ASSERT((first.bytes[8] & UINT8_C(0xc0)) == UINT8_C(0x80));
    DIST_ASSERT(om_distribution_node_id_generate(NULL)
                == OM_STATUS_INVALID_ARGUMENT);
    return true;
}

static void fill_node_id(Om_Distribution_Node_Id *node_id, uint8_t first_byte)
{
    for (size_t index = 0; index < sizeof(node_id->bytes); ++index) {
        node_id->bytes[index] = (uint8_t)(first_byte + (uint8_t)index);
    }
}

static bool test_handshake_frame_and_validation(void)
{
    Om_Distribution_Node_Handshake local = {
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .program_fingerprint = UINT64_C(0x1122334455667788),
        .interface_fingerprint = UINT64_C(0x8877665544332211)
    };
    Om_Distribution_Node_Handshake remote = {0};
    Om_Distribution_Node_Id expected_peer = {0};
    uint8_t payload[16] = {0};
    Om_Distribution_Frame frame = {0};
    fill_node_id(&local.node_id, UINT8_C(0x10));
    fill_node_id(&expected_peer, UINT8_C(0x80));
    DIST_ASSERT(om_distribution_handshake_frame_init(
                   &local, OM_DISTRIBUTION_FRAME_HELLO, payload, &frame)
               == OM_STATUS_OK);
    Om_Distribution_Node_Handshake decoded = {0};
    DIST_ASSERT(om_distribution_handshake_frame_read(&frame, &decoded)
                == OM_STATUS_OK);
    DIST_ASSERT(om_distribution_node_id_equal(decoded.node_id,
                                               local.node_id));
    DIST_ASSERT(decoded.program_fingerprint == local.program_fingerprint);
    DIST_ASSERT(decoded.interface_fingerprint
                == local.interface_fingerprint);

    remote = decoded;
    remote.node_id = expected_peer;
    DIST_ASSERT(om_distribution_handshake_validate(
                   &local, &remote, &expected_peer) == OM_STATUS_OK);
    remote.interface_fingerprint ^= UINT64_C(1);
    DIST_ASSERT(om_distribution_handshake_validate(
                   &local, &remote, &expected_peer)
                == OM_STATUS_INVALID_ARGUMENT);
    remote.interface_fingerprint ^= UINT64_C(1);
    DIST_ASSERT(om_distribution_handshake_validate(
                   &local, &remote, &local.node_id)
                == OM_STATUS_INVALID_ARGUMENT);
    return true;
}

typedef struct Om_Distribution_Handshake_Worker {
    Om_Distribution_Connection *connection;
    Om_Distribution_Node_Handshake local;
    Om_Distribution_Node_Handshake remote;
    Om_Distribution_Node_Id expected_peer;
    bool has_expected_peer;
    Om_Status status;
} Om_Distribution_Handshake_Worker;

static void run_handshake_worker(Om_Distribution_Handshake_Worker *worker)
{
    const Om_Distribution_Node_Id *expected_peer =
        worker->has_expected_peer ? &worker->expected_peer : NULL;
    worker->status = om_distribution_connection_handshake_peer(
        worker->connection, &worker->local, expected_peer, 0,
        &worker->remote);
}

#if defined(_WIN32)
static DWORD WINAPI handshake_worker_entry(void *context)
{
    run_handshake_worker((Om_Distribution_Handshake_Worker *)context);
    return 0;
}
typedef HANDLE Om_Test_Thread;
static bool start_handshake_worker(Om_Test_Thread *thread, void *context)
{
    *thread = CreateThread(NULL, 0, handshake_worker_entry, context, 0, NULL);
    return *thread != NULL;
}
static bool join_handshake_worker(Om_Test_Thread thread)
{
    const DWORD wait_result = WaitForSingleObject(thread, INFINITE);
    const bool joined = wait_result == WAIT_OBJECT_0;
    (void)CloseHandle(thread);
    return joined;
}
#else
static void *handshake_worker_entry(void *context)
{
    run_handshake_worker((Om_Distribution_Handshake_Worker *)context);
    return NULL;
}
typedef pthread_t Om_Test_Thread;
static bool start_handshake_worker(Om_Test_Thread *thread, void *context)
{
    return pthread_create(thread, NULL, handshake_worker_entry, context) == 0;
}
static bool join_handshake_worker(Om_Test_Thread thread)
{
    return pthread_join(thread, NULL) == 0;
}
#endif

static bool test_tcp_node_handshake(bool compatible)
{
    Om_Distribution_Listener listener = {0};
    Om_Distribution_Connection client = {0};
    Om_Distribution_Connection server = {0};
    Om_Distribution_Node_Handshake client_local = {
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .program_fingerprint = UINT64_C(0x123456789abcdef0),
        .interface_fingerprint = UINT64_C(0x0fedcba987654321)
    };
    Om_Distribution_Node_Id server_node = {0};
    Om_Distribution_Handshake_Worker server_worker = {0};
    Om_Test_Thread server_thread = {0};
    bool server_thread_started = false;
    bool passed = false;
    fill_node_id(&client_local.node_id, UINT8_C(0x10));
    fill_node_id(&server_node, UINT8_C(0x80));
    server_worker.local = (Om_Distribution_Node_Handshake){
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .node_id = server_node,
        .program_fingerprint = compatible
                                   ? client_local.program_fingerprint
                                   : client_local.program_fingerprint + 1,
        .interface_fingerprint = client_local.interface_fingerprint
    };
    server_worker.expected_peer = client_local.node_id;
    server_worker.has_expected_peer = true;

    DIST_ASSERT_CLEANUP(om_distribution_listener_open(
                   &listener, (Om_String_View){"127.0.0.1", 9}, 0, 4)
               == OM_STATUS_OK);
    const uint16_t port = om_distribution_listener_port(&listener);
    DIST_ASSERT_CLEANUP(port != 0);
    DIST_ASSERT_CLEANUP(om_distribution_connection_connect(
                   &client, (Om_String_View){"127.0.0.1", 9}, port)
               == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(om_distribution_listener_accept(&listener, &server)
               == OM_STATUS_OK);
    server_worker.connection = &server;
    server_worker.status = OM_STATUS_IO_ERROR;
    DIST_ASSERT_CLEANUP(start_handshake_worker(&server_thread,
                                               &server_worker));
    server_thread_started = true;
    Om_Distribution_Node_Handshake client_remote = {0};
    const Om_Status client_status =
        om_distribution_connection_handshake_peer(
            &client, &client_local, &server_node, 0, &client_remote);
    const bool joined = join_handshake_worker(server_thread);
    server_thread_started = false;
    if (!joined) {
        (void)fprintf(stderr, "handshake worker did not join\n");
        goto cleanup;
    }
    if (compatible) {
        passed = client_status == OM_STATUS_OK
                 && server_worker.status == OM_STATUS_OK;
        passed = passed
                 && om_distribution_node_id_equal(
                        client_remote.node_id, server_node)
                 && om_distribution_node_id_equal(
                        server_worker.remote.node_id, client_local.node_id)
                 && client_remote.program_fingerprint
                        == server_worker.local.program_fingerprint
                 && server_worker.remote.interface_fingerprint
                        == client_local.interface_fingerprint;
        if (passed) {
            static const uint8_t message_payload[] = {0x4f, 0x4d, 0x42, 0x01};
            Om_Distribution_Frame message = {
                .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
                .type = OM_DISTRIBUTION_FRAME_MESSAGE,
                .sender = {.actor_id = UINT64_C(17)},
                .receiver = {.actor_id = UINT64_C(29)},
                .message_fingerprint = UINT64_C(0xa1b2c3d4e5f60718),
                .payload = {message_payload, sizeof(message_payload)}
            };
            Om_Distribution_Frame received = {0};
            message.sender.node = client_local.node_id;
            message.receiver.node = server_node;
            passed = om_distribution_connection_send(&client, &message, 0)
                         == OM_STATUS_OK
                     && om_distribution_connection_receive(
                            &server, 0, &received) == OM_STATUS_OK
                     && received.type == OM_DISTRIBUTION_FRAME_MESSAGE
                     && om_distribution_actor_ref_equal(
                            received.sender, message.sender)
                     && om_distribution_actor_ref_equal(
                            received.receiver, message.receiver)
                     && received.message_fingerprint
                            == message.message_fingerprint
                     && received.payload.length == sizeof(message_payload)
                     && memcmp(received.payload.data, message_payload,
                               sizeof(message_payload)) == 0;
        }
    } else {
        passed = client_status != OM_STATUS_OK
                 && server_worker.status != OM_STATUS_OK
                 && client.owned_state == NULL
                 && server.owned_state == NULL;
    }
    if (!passed) {
        (void)fprintf(stderr,
                      "handshake status mismatch: client=%d server=%d\n",
                      (int)client_status, (int)server_worker.status);
    }

cleanup:
    if (server_thread_started) {
        (void)join_handshake_worker(server_thread);
    }
    om_distribution_connection_close(&server);
    om_distribution_connection_close(&client);
    om_distribution_listener_close(&listener);
    return passed;
}

static bool test_tcp_handshake_compatible(void)
{
    return test_tcp_node_handshake(true);
}

static bool test_tcp_handshake_rejects_mismatch(void)
{
    return test_tcp_node_handshake(false);
}

static bool test_frame_bounds_and_malformed_headers(void)
{
    static const uint8_t payload[] = {1, 2, 3};
    uint8_t encoded[OM_DISTRIBUTION_FRAME_HEADER_SIZE + sizeof(payload)] = {0};
    Om_Distribution_Frame original = make_frame(payload, sizeof(payload));
    size_t written = 99;
    DIST_ASSERT(om_distribution_frame_encode(
                   &original, (Om_Byte_Slice){encoded, sizeof(encoded)},
                   sizeof(encoded) - 1, &written) == OM_STATUS_OUT_OF_RANGE);
    DIST_ASSERT(written == 0);
    DIST_ASSERT(om_distribution_frame_encode(
                   &original, (Om_Byte_Slice){encoded, sizeof(encoded)}, 0,
                   &written) == OM_STATUS_OK);

    Om_Distribution_Frame decoded = {0};
    size_t consumed = 77;
    DIST_ASSERT(om_distribution_frame_decode(
                   (Om_Const_Byte_Slice){encoded, written - 1}, 0,
                   &decoded, &consumed) == OM_STATUS_END_OF_INPUT);
    DIST_ASSERT(consumed == 0);
    DIST_ASSERT(om_distribution_frame_decode(
                   (Om_Const_Byte_Slice){encoded, written},
                   OM_DISTRIBUTION_FRAME_HEADER_SIZE + 2,
                   &decoded, &consumed) == OM_STATUS_OUT_OF_RANGE);

    encoded[0] ^= UINT8_C(0x80);
    DIST_ASSERT(om_distribution_frame_decode(
                   (Om_Const_Byte_Slice){encoded, written}, 0,
                   &decoded, &consumed) == OM_STATUS_INVALID_ARGUMENT);
    encoded[0] ^= UINT8_C(0x80);
    encoded[5] = (uint8_t)(OM_DISTRIBUTION_PROTOCOL_VERSION + 1);
    DIST_ASSERT(om_distribution_frame_decode(
                   (Om_Const_Byte_Slice){encoded, written}, 0,
                   &decoded, &consumed) == OM_STATUS_INVALID_ARGUMENT);
    encoded[5] = (uint8_t)OM_DISTRIBUTION_PROTOCOL_VERSION;
    encoded[7] = UINT8_C(0xff);
    DIST_ASSERT(om_distribution_frame_decode(
                   (Om_Const_Byte_Slice){encoded, written}, 0,
                   &decoded, &consumed) == OM_STATUS_INVALID_ARGUMENT);
    return true;
}

static bool test_message_fingerprint_stability(void)
{
    static const uint64_t fields[] = {1, 2, 3};
    static const uint64_t reordered_fields[] = {1, 3, 2};
    static const uint64_t high_bits_changed[] = {
        UINT64_C(0x100000001), 2, 3
    };
    uint64_t first = 0;
    uint64_t same = 0;
    uint64_t changed = 0;
    uint64_t changed_high_bits = 0;
    DIST_ASSERT(om_distribution_message_fingerprint(
                   (Om_String_View){"actors/counter.bas", 18},
                   (Om_String_View){"Add", 3}, fields, 3,
                   &first) == OM_STATUS_OK);
    DIST_ASSERT(om_distribution_message_fingerprint(
                   (Om_String_View){"actors/counter.bas", 18},
                   (Om_String_View){"Add", 3}, fields, 3,
                   &same) == OM_STATUS_OK);
    DIST_ASSERT(om_distribution_message_fingerprint(
                   (Om_String_View){"actors/counter.bas", 18},
                   (Om_String_View){"Add", 3}, reordered_fields, 3,
                   &changed) == OM_STATUS_OK);
    DIST_ASSERT(om_distribution_message_fingerprint(
                   (Om_String_View){"actors/counter.bas", 18},
                   (Om_String_View){"Add", 3}, high_bits_changed, 3,
                   &changed_high_bits) == OM_STATUS_OK);
    DIST_ASSERT(first == same);
    DIST_ASSERT(first != changed);
    DIST_ASSERT(first != changed_high_bits);
    DIST_ASSERT(om_distribution_message_fingerprint(
                   (Om_String_View){NULL, 0},
                   (Om_String_View){"Add", 3}, fields, 3,
                   &changed) == OM_STATUS_INVALID_ARGUMENT);
    return true;
}

static bool test_malformed_frame_fuzz(void)
{
    uint32_t state = UINT32_C(0x31415926);
    uint8_t bytes[128] = {0};
    for (size_t round = 0; round < 5000; ++round) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        const size_t length = (size_t)(state % (uint32_t)sizeof(bytes));
        for (size_t index = 0; index < length; ++index) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[index] = (uint8_t)(state >> 16);
        }
        Om_Distribution_Frame frame = {0};
        size_t consumed = 0;
        const Om_Status status = om_distribution_frame_decode(
            (Om_Const_Byte_Slice){bytes, length}, 4096, &frame, &consumed);
        DIST_ASSERT(status == OM_STATUS_OK
                    || status == OM_STATUS_INVALID_ARGUMENT
                    || status == OM_STATUS_OUT_OF_RANGE
                    || status == OM_STATUS_END_OF_INPUT);
        if (status == OM_STATUS_OK) {
            DIST_ASSERT(consumed >= OM_DISTRIBUTION_FRAME_HEADER_SIZE);
            DIST_ASSERT(consumed <= length);
            DIST_ASSERT(frame.payload.length
                        == consumed - OM_DISTRIBUTION_FRAME_HEADER_SIZE);
        } else {
            DIST_ASSERT(consumed == 0);
        }
    }
    return true;
}

static bool test_malformed_message_payload_fuzz(void)
{
    uint32_t state = UINT32_C(0x27182818);
    uint8_t bytes[128] = {0};
    for (size_t round = 0; round < 5000; ++round) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        const size_t length = (size_t)(state % (uint32_t)sizeof(bytes));
        for (size_t index = 0; index < length; ++index) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[index] = (uint8_t)(state >> 16);
        }
        Om_Distribution_Message message = {0};
        const Om_Status status = om_distribution_message_deserialize(
            (Om_Const_Byte_Slice){bytes, length}, 1, 1, 0,
            (Om_Distribution_Serializer_Limits){.maximum_payload_bytes = 4096},
            &message);
        DIST_ASSERT(status == OM_STATUS_OK
                    || status == OM_STATUS_INVALID_ARGUMENT
                    || status == OM_STATUS_OUT_OF_RANGE
                    || status == OM_STATUS_END_OF_INPUT);
        if (status == OM_STATUS_OK) {
            DIST_ASSERT(message.field_count == 0);
            om_distribution_message_destroy(&message);
        }
    }
    return true;
}

static bool test_tcp_ordered_frames_at_most_once(void)
{
    static const uint8_t first_payload[] = {0x11, 0x12};
    static const uint8_t second_payload[] = {0x21, 0x22, 0x23};
    Om_Distribution_Listener listener = {0};
    Om_Distribution_Connection client = {0};
    Om_Distribution_Connection server = {0};
    Om_Distribution_Frame received = {0};
    bool passed = false;
    DIST_ASSERT_CLEANUP(om_distribution_listener_open(
                   &listener, (Om_String_View){"127.0.0.1", 9}, 0, 4)
               == OM_STATUS_OK);
    const uint16_t port = om_distribution_listener_port(&listener);
    DIST_ASSERT_CLEANUP(port != 0);
    DIST_ASSERT_CLEANUP(om_distribution_connection_connect(
                   &client, (Om_String_View){"127.0.0.1", 9}, port)
               == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(om_distribution_listener_accept(&listener, &server)
                == OM_STATUS_OK);

    Om_Distribution_Frame first = make_frame(
        first_payload, sizeof(first_payload));
    Om_Distribution_Frame second = make_frame(
        second_payload, sizeof(second_payload));
    first.message_fingerprint = UINT64_C(0x101);
    second.message_fingerprint = UINT64_C(0x202);
    DIST_ASSERT_CLEANUP(om_distribution_connection_send(&client, &first, 0)
                == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(om_distribution_connection_send(&client, &second, 0)
                == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(om_distribution_connection_receive(&server, 0, &received)
                == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(received.message_fingerprint == UINT64_C(0x101));
    DIST_ASSERT_CLEANUP(received.payload.length == sizeof(first_payload));
    DIST_ASSERT_CLEANUP(memcmp(received.payload.data, first_payload,
                       sizeof(first_payload)) == 0);
    DIST_ASSERT_CLEANUP(om_distribution_connection_receive(&server, 0, &received)
                == OM_STATUS_OK);
    DIST_ASSERT_CLEANUP(received.message_fingerprint == UINT64_C(0x202));
    DIST_ASSERT_CLEANUP(received.payload.length == sizeof(second_payload));
    DIST_ASSERT_CLEANUP(memcmp(received.payload.data, second_payload,
                       sizeof(second_payload)) == 0);
    om_distribution_connection_close(&client);
    DIST_ASSERT_CLEANUP(om_distribution_connection_receive(&server, 0, &received)
                == OM_STATUS_END_OF_INPUT);
    passed = true;

cleanup:
    om_distribution_connection_close(&server);
    om_distribution_connection_close(&client);
    om_distribution_listener_close(&listener);
    return passed;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"distribution_frame_roundtrip", test_frame_roundtrip_and_stream_consumption},
        {"distribution_node_identity", test_node_identity_generation},
        {"distribution_handshake_frame",
         test_handshake_frame_and_validation},
        {"distribution_message_payload_roundtrip",
         test_message_payload_serializer_roundtrip},
        {"distribution_large_nested_payload_budget",
         test_large_nested_payload_budget},
        {"distribution_message_payload_rejections",
         test_message_payload_serializer_rejections},
        {"distribution_frame_bounds", test_frame_bounds_and_malformed_headers},
        {"distribution_message_fingerprint", test_message_fingerprint_stability},
        {"distribution_malformed_frame_fuzz", test_malformed_frame_fuzz},
        {"distribution_malformed_message_fuzz",
         test_malformed_message_payload_fuzz},
        {"distribution_tcp_ordered_at_most_once",
         test_tcp_ordered_frames_at_most_once},
        {"distribution_tcp_handshake_compatible",
         test_tcp_handshake_compatible},
        {"distribution_tcp_handshake_rejects_mismatch",
         test_tcp_handshake_rejects_mismatch}
    };
    size_t failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (tests[index].function()) {
            (void)printf("PASS %s\n", tests[index].name);
        } else {
            (void)fprintf(stderr, "FAIL %s\n", tests[index].name);
            ++failures;
        }
    }
    (void)printf("%zu tests, %zu failures\n",
                 sizeof(tests) / sizeof(tests[0]), failures);
    return failures == 0 ? 0 : 1;
}
