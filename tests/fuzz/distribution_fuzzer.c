#include <stddef.h>
#include <stdint.h>

#include "ombasic/distribution/protocol.h"
#include "ombasic/distribution/serializer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const size_t maximum_input_size = 1024 * 1024;
    if (size > maximum_input_size) return 0;

    const Om_Const_Byte_Slice input = {data, size};
    Om_Distribution_Frame frame = {0};
    size_t consumed = 0;
    (void)om_distribution_frame_size_from_header(input, maximum_input_size,
                                                  &consumed);
    (void)om_distribution_frame_decode(input, maximum_input_size, &frame,
                                       &consumed);

    const Om_Distribution_Serializer_Limits limits = {
        .maximum_payload_bytes = maximum_input_size,
        .maximum_collection_length = 4096,
        .maximum_nesting_depth = 32,
        .maximum_total_values = 8192,
    };
    for (size_t expected_fields = 0; expected_fields <= 16;
         ++expected_fields) {
        Om_Distribution_Message message = {0};
        (void)om_distribution_message_deserialize(input,
            UINT64_C(0x4f4d42465a5a4152), UINT64_C(0x4f4d42465a5a4152),
            expected_fields, limits, &message);
        om_distribution_message_destroy(&message);
    }
    if (frame.type == OM_DISTRIBUTION_FRAME_MESSAGE) {
        for (size_t expected_fields = 0; expected_fields <= 16;
             ++expected_fields) {
            Om_Distribution_Message message = {0};
            (void)om_distribution_message_deserialize(frame.payload,
                frame.message_fingerprint, frame.message_fingerprint,
                expected_fields, limits, &message);
            om_distribution_message_destroy(&message);
        }
    }
    return 0;
}
