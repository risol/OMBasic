#include <stddef.h>
#include <stdint.h>

#include "ombasic/source/source_text.h"
#include "ombasic/syntax/lexer.h"
#include "ombasic/syntax/parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char path[] = "fuzz.bas";
    const size_t maximum_input_size = 1024 * 1024;
    if (size > maximum_input_size) return 0;

    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    const Om_Status source_status = om_source_text_init(&source,
        (Om_Source_Id){1}, (Om_String_View){path, sizeof(path) - 1},
        (Om_Const_Byte_Slice){data, size}, maximum_input_size);
    if (source_status == OM_STATUS_OK) {
        if (om_lexer_scan(&source, &lexed) == OM_STATUS_OK) {
            (void)om_parser_parse(&lexed, &parsed);
        }
    }

    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    return 0;
}
