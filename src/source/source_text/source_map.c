#include "ombasic/source/source_text.h"

Om_Status om_source_text_location(
    const Om_Source_Text *source,
    uint32_t byte_offset,
    Om_Source_Location *location
)
{
    size_t low = 0;
    size_t high = 0;
    if (location == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *location = (Om_Source_Location){0};
    if (source == NULL || source->owned_storage == NULL
        || source->line_starts == NULL || source->line_count == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (byte_offset > source->bytes.length) {
        return OM_STATUS_OUT_OF_RANGE;
    }

    high = source->line_count;
    while (low + 1 < high) {
        const size_t middle = low + (high - low) / 2;
        if (source->line_starts[middle] <= byte_offset) {
            low = middle;
        } else {
            high = middle;
        }
    }
    location->line = (uint32_t)(low + 1);
    location->byte_column = byte_offset - source->line_starts[low] + 1;
    return OM_STATUS_OK;
}

Om_Status om_source_text_slice(
    const Om_Source_Text *source,
    Om_Source_Span span,
    Om_Const_Byte_Slice *slice
)
{
    if (slice == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *slice = (Om_Const_Byte_Slice){0};
    if (source == NULL || source->owned_storage == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (span.source.value != source->id.value) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (!om_source_span_is_valid(span, (uint32_t)source->bytes.length)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    slice->data = source->bytes.data + span.start;
    slice->length = (size_t)(span.end - span.start);
    return OM_STATUS_OK;
}
