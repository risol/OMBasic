#ifndef OMBASIC_SYNTAX_PARSER_H
#define OMBASIC_SYNTAX_PARSER_H

#include <stddef.h>

#include "ombasic/foundation/status.h"
#include "ombasic/syntax/ast.h"

typedef enum Om_Parse_Diagnostic_Code {
    /* E0201 is reserved by the development plan for invalid import paths. */
    OM_PARSE_DIAGNOSTIC_E0202 = 2,
    OM_PARSE_DIAGNOSTIC_E0203 = 3,
    OM_PARSE_DIAGNOSTIC_E0204 = 4,
    OM_PARSE_DIAGNOSTIC_E0205 = 5,
    OM_PARSE_DIAGNOSTIC_E0206 = 6,
    OM_PARSE_DIAGNOSTIC_E0207 = 7,
    OM_PARSE_DIAGNOSTIC_E0208 = 8
} Om_Parse_Diagnostic_Code;

typedef struct Om_Parse_Diagnostic {
    Om_Parse_Diagnostic_Code code;
    Om_Source_Span span;
    Om_Token_Kind expected_kind;
    Om_Keyword expected_keyword;
} Om_Parse_Diagnostic;

typedef struct Om_Parser_Result {
    Om_Ast ast;
    Om_Parse_Diagnostic *diagnostics;
    size_t diagnostic_count;
} Om_Parser_Result;

/* Tokens and all name/lexeme views must outlive the result AST. Pass zero-init. */
Om_Status om_parser_parse(const Om_Lexer_Result *lexed, Om_Parser_Result *result);
void om_parser_result_destroy(Om_Parser_Result *result);
const char *om_parse_diagnostic_code_name(Om_Parse_Diagnostic_Code code);

#endif
