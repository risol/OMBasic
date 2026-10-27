#ifndef OMBASIC_TOOLING_DIAGNOSTICS_JSON_H
#define OMBASIC_TOOLING_DIAGNOSTICS_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/source/source_text.h"

typedef struct Om_Tooling_Diagnostic {
    Om_String_View stage;
    Om_String_View code;
    Om_String_View message;
    const Om_Source_Text *source;
    Om_Source_Span span;
    bool has_related_span;
    const Om_Source_Text *related_source;
    Om_Source_Span related_span;
} Om_Tooling_Diagnostic;

typedef struct Om_Tooling_Json_Result {
    char *data;
    size_t length;
} Om_Tooling_Json_Result;

/* Emits stable one-based line and UTF-8 byte-column ranges. */
Om_Status om_tooling_diagnostics_to_json(
    const Om_Tooling_Diagnostic *diagnostics,
    size_t diagnostic_count,
    Om_Tooling_Json_Result *result
);
void om_tooling_json_result_destroy(Om_Tooling_Json_Result *result);

#endif
