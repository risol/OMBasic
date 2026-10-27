#ifndef OMBASIC_SOURCE_SOURCE_TEXT_H
#define OMBASIC_SOURCE_SOURCE_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/ids.h"
#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/source/span.h"

#define OM_SOURCE_TEXT_DEFAULT_MAX_BYTES ((size_t)16 * (size_t)1024 * (size_t)1024)

/*
 * Owns immutable UTF-8 path bytes, source bytes, and a line-start table.
 * Initialize from {0}; access data through the const views and destroy once.
 */
typedef struct Om_Source_Text {
    Om_Source_Id id;
    Om_String_View canonical_path;
    Om_Const_Byte_Slice bytes;
    const uint32_t *line_starts;
    uint32_t line_count;
    uint64_t content_hash;
    void *owned_storage;
} Om_Source_Text;

typedef struct Om_Source_Location {
    uint32_t line;
    uint32_t byte_column;
} Om_Source_Location;

Om_Status om_source_text_init(
    Om_Source_Text *source,
    Om_Source_Id id,
    Om_String_View canonical_path,
    Om_Const_Byte_Slice input,
    size_t maximum_byte_length
);
void om_source_text_destroy(Om_Source_Text *source);
Om_Status om_source_text_location(
    const Om_Source_Text *source,
    uint32_t byte_offset,
    Om_Source_Location *location
);
Om_Status om_source_text_slice(
    const Om_Source_Text *source,
    Om_Source_Span span,
    Om_Const_Byte_Slice *slice
);

#endif
