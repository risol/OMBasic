#ifndef OMBASIC_TOOLING_FORMATTER_H
#define OMBASIC_TOOLING_FORMATTER_H

#include <stddef.h>

#include "ombasic/foundation/status.h"
#include "ombasic/source/source_text.h"
#include "ombasic/syntax/parser.h"

typedef struct Om_Formatter_Result {
    char *data;
    size_t length;
} Om_Formatter_Result;

/* Formats a successfully lexed and parsed module deterministically. */
Om_Status om_formatter_format(
    const Om_Source_Text *source,
    const Om_Lexer_Result *lexed,
    const Om_Parser_Result *parsed,
    Om_Formatter_Result *result
);
void om_formatter_result_destroy(Om_Formatter_Result *result);

#endif
