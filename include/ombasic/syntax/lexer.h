#ifndef OMBASIC_SYNTAX_LEXER_H
#define OMBASIC_SYNTAX_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/source/source_text.h"

typedef enum Om_Token_Kind {
    OM_TOKEN_ERROR = 0,
    OM_TOKEN_IDENTIFIER,
    OM_TOKEN_ENTITY_IDENTIFIER,
    OM_TOKEN_INVALID_IDENTIFIER,
    OM_TOKEN_KEYWORD,
    OM_TOKEN_INTEGER,
    OM_TOKEN_FLOAT,
    OM_TOKEN_STRING,
    OM_TOKEN_INTERPOLATED_STRING_START,
    OM_TOKEN_STRING_TEXT,
    OM_TOKEN_INTERPOLATION_START,
    OM_TOKEN_INTERPOLATION_END,
    OM_TOKEN_INTERPOLATED_STRING_END,
    OM_TOKEN_DOC_COMMENT,
    OM_TOKEN_NEWLINE,
    OM_TOKEN_PLUS,
    OM_TOKEN_MINUS,
    OM_TOKEN_STAR,
    OM_TOKEN_SLASH,
    OM_TOKEN_PERCENT,
    OM_TOKEN_EQUAL,
    OM_TOKEN_NOT_EQUAL,
    OM_TOKEN_LESS,
    OM_TOKEN_GREATER,
    OM_TOKEN_LESS_EQUAL,
    OM_TOKEN_GREATER_EQUAL,
    OM_TOKEN_DOT,
    OM_TOKEN_LEFT_PAREN,
    OM_TOKEN_RIGHT_PAREN,
    OM_TOKEN_LEFT_BRACKET,
    OM_TOKEN_RIGHT_BRACKET,
    OM_TOKEN_LEFT_BRACE,
    OM_TOKEN_RIGHT_BRACE,
    OM_TOKEN_COMMA,
    OM_TOKEN_COLON,
    OM_TOKEN_EOF
} Om_Token_Kind;

typedef enum Om_Keyword {
    OM_KEYWORD_NONE = 0,
    OM_KEYWORD_ACTION,
    OM_KEYWORD_ACTOR,
    OM_KEYWORD_AND,
    OM_KEYWORD_AT,
    OM_KEYWORD_BREAK,
    OM_KEYWORD_CASE,
    OM_KEYWORD_CONTINUE,
    OM_KEYWORD_DESCENDING,
    OM_KEYWORD_ELSE,
    OM_KEYWORD_END,
    OM_KEYWORD_EXPORT,
    OM_KEYWORD_FALSE,
    OM_KEYWORD_FOR,
    OM_KEYWORD_FROM,
    OM_KEYWORD_FUNCTION,
    OM_KEYWORD_IF,
    OM_KEYWORD_IMPORT,
    OM_KEYWORD_IN,
    OM_KEYWORD_MESSAGE,
    OM_KEYWORD_NATIVE,
    OM_KEYWORD_NOT,
    OM_KEYWORD_NULL,
    OM_KEYWORD_OR,
    OM_KEYWORD_RECORD,
    OM_KEYWORD_RECEIVE,
    OM_KEYWORD_RETURN,
    OM_KEYWORD_QUERY,
    OM_KEYWORD_ORDER,
    OM_KEYWORD_BY,
    OM_KEYWORD_TAKE,
    OM_KEYWORD_SELECT,
    OM_KEYWORD_ASCENDING,
    OM_KEYWORD_WHERE,
    OM_KEYWORD_SELF,
    OM_KEYWORD_SEND,
    OM_KEYWORD_SPAWN,
    OM_KEYWORD_STOP,
    OM_KEYWORD_STEP,
    OM_KEYWORD_THEN,
    OM_KEYWORD_TO,
    OM_KEYWORD_TRUE,
    OM_KEYWORD_TRY,
    OM_KEYWORD_VAR,
    OM_KEYWORD_WATCH,
    OM_KEYWORD_WHILE,
    OM_KEYWORD_WITH
} Om_Keyword;

typedef enum Om_Lex_Diagnostic_Code {
    OM_LEX_DIAGNOSTIC_E0101 = 1,
    OM_LEX_DIAGNOSTIC_E0102,
    OM_LEX_DIAGNOSTIC_E0103,
    OM_LEX_DIAGNOSTIC_E0104
} Om_Lex_Diagnostic_Code;

typedef struct Om_Token {
    Om_Token_Kind kind;
    Om_Keyword keyword;
    Om_Source_Span span;
    Om_String_View lexeme;
    uint64_t integer_magnitude;
    double float_value;
    /* True only for magnitude 2^63; the parser must require a direct unary minus. */
    bool integer_requires_negative;
} Om_Token;

typedef struct Om_Lex_Diagnostic {
    Om_Lex_Diagnostic_Code code;
    Om_Source_Span span;
} Om_Lex_Diagnostic;

typedef struct Om_Lexer_Result {
    Om_Token *tokens;
    size_t token_count;
    Om_Lex_Diagnostic *diagnostics;
    size_t diagnostic_count;
} Om_Lexer_Result;

/* Pass a zero-initialized result. Token lexemes borrow source bytes; destroy once. */
Om_Status om_lexer_scan(const Om_Source_Text *source, Om_Lexer_Result *result);
void om_lexer_result_destroy(Om_Lexer_Result *result);
const char *om_lex_diagnostic_code_name(Om_Lex_Diagnostic_Code code);

#endif
