#include "ombasic/syntax/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"

typedef struct Om_Parser {
    const Om_Lexer_Result *lexed;
    size_t current;
    Om_Vector nodes;
    Om_Vector children;
    Om_Vector diagnostics;
    unsigned expression_depth;
    unsigned block_depth;
} Om_Parser;

static Om_Status parse_expression(Om_Parser *parser, unsigned minimum_precedence,
                                  Om_Ast_Node_Id *out);
static Om_Status parse_expression_impl(Om_Parser *parser, unsigned minimum_precedence,
                                       Om_Ast_Node_Id *out);
static Om_Status parse_block(Om_Parser *parser, Om_Ast_Node_Id *out);
static Om_Status parse_block_impl(Om_Parser *parser, Om_Ast_Node_Id *out);
static Om_Status parse_statement(Om_Parser *parser, Om_Ast_Node_Id *out);
static Om_Status parse_query_expression(Om_Parser *parser,
                                        Om_Ast_Node_Id *out);

static const Om_Token *peek(const Om_Parser *parser)
{
    size_t index = parser->current;
    if (index >= parser->lexed->token_count) {
        index = parser->lexed->token_count - 1;
    }
    return &parser->lexed->tokens[index];
}

static const Om_Token *peek_next(const Om_Parser *parser)
{
    size_t index = parser->current;
    if (index < parser->lexed->token_count - 1) {
        ++index;
    }
    return &parser->lexed->tokens[index];
}

static const Om_Token *advance(Om_Parser *parser)
{
    const Om_Token *token = peek(parser);
    if (token->kind != OM_TOKEN_EOF && parser->current < parser->lexed->token_count) {
        ++parser->current;
    }
    return token;
}

static bool at_kind(const Om_Parser *parser, Om_Token_Kind kind)
{
    return peek(parser)->kind == kind;
}

static bool at_keyword(const Om_Parser *parser, Om_Keyword keyword)
{
    const Om_Token *token = peek(parser);
    return token->kind == OM_TOKEN_KEYWORD && token->keyword == keyword;
}

static bool match_keyword(Om_Parser *parser, Om_Keyword keyword)
{
    if (!at_keyword(parser, keyword)) {
        return false;
    }
    (void)advance(parser);
    return true;
}

static bool is_name_token(const Om_Token *token)
{
    return token->kind == OM_TOKEN_IDENTIFIER
           || token->kind == OM_TOKEN_ENTITY_IDENTIFIER
           || token->kind == OM_TOKEN_INVALID_IDENTIFIER;
}

static bool is_expression_name(const Om_Token *token)
{
    return is_name_token(token);
}

static Om_Status add_diagnostic(Om_Parser *parser, Om_Parse_Diagnostic_Code code,
                                Om_Source_Span span, Om_Token_Kind expected_kind,
                                Om_Keyword expected_keyword)
{
    const Om_Parse_Diagnostic diagnostic = {
        .code = code,
        .span = span,
        .expected_kind = expected_kind,
        .expected_keyword = expected_keyword,
    };
    return om_vector_push(&parser->diagnostics, &diagnostic);
}

static Om_Status expect_kind(Om_Parser *parser, Om_Token_Kind kind)
{
    if (at_kind(parser, kind)) {
        (void)advance(parser);
        return OM_STATUS_OK;
    }
    return add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202, peek(parser)->span,
                          kind, OM_KEYWORD_NONE);
}

static Om_Status expect_keyword(Om_Parser *parser, Om_Keyword keyword)
{
    if (at_keyword(parser, keyword)) {
        (void)advance(parser);
        return OM_STATUS_OK;
    }
    return add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202, peek(parser)->span,
                          OM_TOKEN_KEYWORD, keyword);
}

static Om_Status expect_name(Om_Parser *parser, bool entity, Om_Token *out)
{
    const Om_Token *token = peek(parser);
    if (token->kind == OM_TOKEN_IDENTIFIER
        || token->kind == OM_TOKEN_ENTITY_IDENTIFIER
        || token->kind == OM_TOKEN_INVALID_IDENTIFIER) {
        const bool wrong_category = token->kind != OM_TOKEN_INVALID_IDENTIFIER
            && (entity ? token->kind != OM_TOKEN_ENTITY_IDENTIFIER
                       : token->kind != OM_TOKEN_IDENTIFIER);
        *out = *advance(parser);
        return wrong_category
            ? add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0102, token->span,
                             OM_TOKEN_INVALID_IDENTIFIER, OM_KEYWORD_NONE)
            : OM_STATUS_OK;
    }
    return add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202, token->span,
                          entity ? OM_TOKEN_ENTITY_IDENTIFIER : OM_TOKEN_IDENTIFIER,
                          OM_KEYWORD_NONE);
}

static Om_Source_Span span_between(Om_Source_Span first, Om_Source_Span last)
{
    return (Om_Source_Span){first.source, first.start, last.end};
}

static Om_Source_Span current_point(const Om_Parser *parser)
{
    const Om_Source_Span span = peek(parser)->span;
    return (Om_Source_Span){span.source, span.start, span.start};
}

static Om_Status make_node(Om_Parser *parser, Om_Ast_Kind kind, Om_Source_Span span,
                           Om_String_View name, Om_Token_Kind token_kind,
                           Om_Keyword keyword, uint32_t flags, uint32_t auxiliary,
                           uint64_t integer_magnitude, double float_value,
                           const Om_Ast_Node_Id *children, size_t child_count,
                           Om_Ast_Node_Id *out)
{
    if (parser->nodes.length >= UINT32_MAX || parser->children.length > UINT32_MAX
        || child_count > UINT32_MAX - parser->children.length) {
        return OM_STATUS_OVERFLOW;
    }
    const uint32_t first_child = (uint32_t)parser->children.length;
    for (size_t index = 0; index < child_count; ++index) {
        const Om_Status status = om_vector_push(&parser->children, &children[index]);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    const Om_Ast_Node node = {
        .kind = kind,
        .span = span,
        .name = name,
        .token_kind = token_kind,
        .keyword = keyword,
        .flags = flags,
        .auxiliary = auxiliary,
        .first_child = first_child,
        .child_count = (uint32_t)child_count,
        .integer_magnitude = integer_magnitude,
        .float_value = float_value,
    };
    const Om_Status status = om_vector_push(&parser->nodes, &node);
    if (status != OM_STATUS_OK) {
        return status;
    }
    *out = (Om_Ast_Node_Id){(uint32_t)(parser->nodes.length - 1)};
    return OM_STATUS_OK;
}

static const Om_Ast_Node *node_at(const Om_Parser *parser, Om_Ast_Node_Id id)
{
    return id.value < parser->nodes.length
               ? (const Om_Ast_Node *)om_vector_at_const(&parser->nodes, id.value)
               : NULL;
}

static Om_Status make_simple_node(Om_Parser *parser, Om_Ast_Kind kind,
                                  Om_Source_Span span, Om_Ast_Node_Id *out)
{
    return make_node(parser, kind, span, (Om_String_View){NULL, 0}, OM_TOKEN_ERROR,
                     OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0, NULL, 0, out);
}

static Om_Status init_id_list(Om_Vector *list)
{
    return om_vector_init(list, sizeof(Om_Ast_Node_Id));
}

static Om_Status push_id(Om_Vector *list, Om_Ast_Node_Id id)
{
    return om_vector_push(list, &id);
}

static Om_Status push_list(Om_Vector *destination, const Om_Vector *source)
{
    for (size_t index = 0; index < source->length; ++index) {
        const Om_Ast_Node_Id *id = om_vector_at_const(source, index);
        const Om_Status status = push_id(destination, *id);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static bool is_block_terminator(const Om_Parser *parser)
{
    return at_kind(parser, OM_TOKEN_EOF) || at_keyword(parser, OM_KEYWORD_END)
           || at_keyword(parser, OM_KEYWORD_ELSE) || at_keyword(parser, OM_KEYWORD_CASE);
}

static void skip_newlines_and_docs(Om_Parser *parser)
{
    while (at_kind(parser, OM_TOKEN_NEWLINE) || at_kind(parser, OM_TOKEN_DOC_COMMENT)) {
        (void)advance(parser);
    }
}

static Om_Status consume_line_end(Om_Parser *parser)
{
    if (at_kind(parser, OM_TOKEN_EOF)) {
        return OM_STATUS_OK;
    }
    if (!at_kind(parser, OM_TOKEN_NEWLINE)) {
        const Om_Status status = add_diagnostic(
            parser, OM_PARSE_DIAGNOSTIC_E0202, peek(parser)->span,
            OM_TOKEN_NEWLINE, OM_KEYWORD_NONE
        );
        if (status != OM_STATUS_OK) {
            return status;
        }
        while (!at_kind(parser, OM_TOKEN_NEWLINE) && !at_kind(parser, OM_TOKEN_EOF)
               && !is_block_terminator(parser)) {
            (void)advance(parser);
        }
    }
    while (at_kind(parser, OM_TOKEN_NEWLINE)) {
        (void)advance(parser);
    }
    return OM_STATUS_OK;
}

static uint32_t flags_for_name(const Om_Token *token)
{
    uint32_t flags = OM_AST_FLAG_NONE;
    if (token->kind == OM_TOKEN_ENTITY_IDENTIFIER) {
        flags |= OM_AST_FLAG_ENTITY_NAME;
    }
    if (token->kind == OM_TOKEN_INVALID_IDENTIFIER) {
        flags |= OM_AST_FLAG_INVALID_NAME;
    }
    return flags;
}

static Om_Status make_name_node(Om_Parser *parser, const Om_Token *token,
                                Om_Ast_Node_Id *out)
{
    return make_node(parser, OM_AST_NAME, token->span, token->lexeme,
                     token->kind, OM_KEYWORD_NONE, flags_for_name(token), 0,
                     0, 0.0, NULL, 0, out);
}

static Om_Status make_parameter(Om_Parser *parser, const Om_Token *token,
                                Om_Ast_Node_Id *out)
{
    return make_node(parser, OM_AST_PARAMETER, token->span, token->lexeme,
                     token->kind, OM_KEYWORD_NONE, flags_for_name(token), 0,
                     0, 0.0, NULL, 0, out);
}

static Om_Status parse_expression_list(Om_Parser *parser, Om_Vector *items,
                                       Om_Token_Kind terminator)
{
    Om_Status status = init_id_list(items);
    if (status != OM_STATUS_OK || at_kind(parser, terminator)) {
        return status;
    }
    for (;;) {
        Om_Ast_Node_Id expression = OM_AST_NODE_ID_INVALID;
        status = parse_expression(parser, 1, &expression);
        if (status != OM_STATUS_OK) {
            return status;
        }
        status = push_id(items, expression);
        if (status != OM_STATUS_OK || !at_kind(parser, OM_TOKEN_COMMA)) {
            return status;
        }
        (void)advance(parser);
        if (at_kind(parser, terminator)) {
            return add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0204,
                                  peek(parser)->span, OM_TOKEN_IDENTIFIER,
                                  OM_KEYWORD_NONE);
        }
    }
}

static Om_Status parse_integer_node(Om_Parser *parser, bool allow_minimum,
                                    Om_Ast_Node_Id *out)
{
    const Om_Token *token = advance(parser);
    uint32_t flags = OM_AST_FLAG_NONE;
    Om_Status status = OM_STATUS_OK;
    if (token->integer_requires_negative) {
        if (allow_minimum) {
            flags |= OM_AST_FLAG_MIN_INTEGER;
        } else {
            status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0205,
                                    token->span, OM_TOKEN_ERROR, OM_KEYWORD_NONE);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return make_node(parser, OM_AST_INTEGER_LITERAL, token->span, token->lexeme,
                     token->kind, OM_KEYWORD_NONE, flags, 0,
                     token->integer_magnitude, 0.0, NULL, 0, out);
}

static Om_Status parse_primary(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *token = peek(parser);
    if (at_keyword(parser, OM_KEYWORD_QUERY)) {
        return parse_query_expression(parser, out);
    }
    if (token->kind == OM_TOKEN_INTEGER) {
        return parse_integer_node(parser, false, out);
    }
    if (token->kind == OM_TOKEN_FLOAT) {
        (void)advance(parser);
        return make_node(parser, OM_AST_FLOAT_LITERAL, token->span, token->lexeme,
                         token->kind, OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0,
                         0, token->float_value, NULL, 0, out);
    }
    if (token->kind == OM_TOKEN_STRING) {
        (void)advance(parser);
        return make_node(parser, OM_AST_STRING_LITERAL, token->span, token->lexeme,
                         token->kind, OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0,
                         0, 0.0, NULL, 0, out);
    }
    if (is_expression_name(token)) {
        (void)advance(parser);
        return make_name_node(parser, token, out);
    }
    if (token->kind == OM_TOKEN_INTERPOLATED_STRING_START) {
        (void)advance(parser);
        Om_Vector parts = {0};
        Om_Status status = init_id_list(&parts);
        if (status != OM_STATUS_OK) {
            return status;
        }
        while (!at_kind(parser, OM_TOKEN_INTERPOLATED_STRING_END)
               && !at_kind(parser, OM_TOKEN_EOF)) {
            if (at_kind(parser, OM_TOKEN_STRING_TEXT)) {
                const Om_Token *text = advance(parser);
                Om_Ast_Node_Id part = OM_AST_NODE_ID_INVALID;
                status = make_node(parser, OM_AST_STRING_TEXT, text->span,
                                   text->lexeme, text->kind, OM_KEYWORD_NONE,
                                   OM_AST_FLAG_NONE, 0, 0, 0.0, NULL, 0, &part);
                if (status == OM_STATUS_OK) {
                    status = push_id(&parts, part);
                }
            } else if (at_kind(parser, OM_TOKEN_INTERPOLATION_START)) {
                (void)advance(parser);
                Om_Ast_Node_Id part = OM_AST_NODE_ID_INVALID;
                status = parse_expression(parser, 1, &part);
                if (status == OM_STATUS_OK) {
                    status = push_id(&parts, part);
                }
                if (status == OM_STATUS_OK && at_kind(parser, OM_TOKEN_INTERPOLATION_END)) {
                    (void)advance(parser);
                } else if (status == OM_STATUS_OK) {
                    status = expect_kind(parser, OM_TOKEN_INTERPOLATION_END);
                }
            } else {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                                        peek(parser)->span,
                                        OM_TOKEN_STRING_TEXT,
                                        OM_KEYWORD_NONE);
                if (status == OM_STATUS_OK) {
                    (void)advance(parser);
                }
            }
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&parts);
                return status;
            }
        }
        const Om_Token *end = peek(parser);
        if (at_kind(parser, OM_TOKEN_INTERPOLATED_STRING_END)) {
            end = advance(parser);
        } else {
            status = expect_kind(parser, OM_TOKEN_INTERPOLATED_STRING_END);
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&parts);
                return status;
            }
        }
        status = make_node(parser, OM_AST_INTERPOLATED_STRING,
                           span_between(token->span, end->span),
                           (Om_String_View){NULL, 0}, token->kind,
                           OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                           (const Om_Ast_Node_Id *)parts.data, parts.length, out);
        om_vector_destroy(&parts);
        return status;
    }
    if (at_keyword(parser, OM_KEYWORD_TRUE) || at_keyword(parser, OM_KEYWORD_FALSE)) {
        (void)advance(parser);
        return make_node(parser, OM_AST_BOOLEAN_LITERAL, token->span, token->lexeme,
                         token->kind, token->keyword, OM_AST_FLAG_NONE, 0,
                         0, 0.0, NULL, 0, out);
    }
    if (at_keyword(parser, OM_KEYWORD_NULL)) {
        (void)advance(parser);
        return make_node(parser, OM_AST_NULL_LITERAL, token->span, token->lexeme,
                         token->kind, token->keyword, OM_AST_FLAG_NONE, 0,
                         0, 0.0, NULL, 0, out);
    }
    if (at_keyword(parser, OM_KEYWORD_SELF)) {
        (void)advance(parser);
        return make_node(parser, OM_AST_SELF, token->span, token->lexeme,
                         token->kind, token->keyword, OM_AST_FLAG_NONE, 0,
                         0, 0.0, NULL, 0, out);
    }
    if (at_kind(parser, OM_TOKEN_LEFT_PAREN)) {
        const Om_Token *open = advance(parser);
        Om_Ast_Node_Id expression = OM_AST_NODE_ID_INVALID;
        Om_Status status = parse_expression(parser, 1, &expression);
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Token *close = peek(parser);
        if (at_kind(parser, OM_TOKEN_RIGHT_PAREN)) {
            close = advance(parser);
        } else {
            status = expect_kind(parser, OM_TOKEN_RIGHT_PAREN);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        return make_node(parser, OM_AST_GROUP, span_between(open->span, close->span),
                         (Om_String_View){NULL, 0}, OM_TOKEN_LEFT_PAREN,
                         OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                         &expression, 1, out);
    }
    if (at_kind(parser, OM_TOKEN_LEFT_BRACKET)) {
        const Om_Token *open = advance(parser);
        Om_Vector elements = {0};
        Om_Status status = parse_expression_list(parser, &elements,
                                                 OM_TOKEN_RIGHT_BRACKET);
        if (status != OM_STATUS_OK) {
            om_vector_destroy(&elements);
            return status;
        }
        const Om_Token *close = peek(parser);
        if (at_kind(parser, OM_TOKEN_RIGHT_BRACKET)) {
            close = advance(parser);
        } else {
            status = expect_kind(parser, OM_TOKEN_RIGHT_BRACKET);
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&elements);
                return status;
            }
        }
        status = make_node(parser, OM_AST_LIST, span_between(open->span, close->span),
                           (Om_String_View){NULL, 0}, OM_TOKEN_LEFT_BRACKET,
                           OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                           (const Om_Ast_Node_Id *)elements.data,
                           elements.length, out);
        om_vector_destroy(&elements);
        return status;
    }
    if (at_kind(parser, OM_TOKEN_LEFT_BRACE)) {
        const Om_Token *open = advance(parser);
        Om_Vector entries = {0};
        Om_Status status = init_id_list(&entries);
        if (status != OM_STATUS_OK) {
            return status;
        }
        while (!at_kind(parser, OM_TOKEN_RIGHT_BRACE) && !at_kind(parser, OM_TOKEN_EOF)) {
            const size_t before = parser->current;
            Om_Ast_Node_Id key = OM_AST_NODE_ID_INVALID;
            Om_Ast_Node_Id value = OM_AST_NODE_ID_INVALID;
            Om_Ast_Node_Id entry = OM_AST_NODE_ID_INVALID;
            status = parse_expression(parser, 1, &key);
            if (status == OM_STATUS_OK && at_kind(parser, OM_TOKEN_COLON)) {
                (void)advance(parser);
            } else if (status == OM_STATUS_OK) {
                status = expect_kind(parser, OM_TOKEN_COLON);
            }
            if (status == OM_STATUS_OK) {
                status = parse_expression(parser, 1, &value);
            }
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node *key_node = node_at(parser, key);
                const Om_Ast_Node *value_node = node_at(parser, value);
                const Om_Source_Span entry_span = key_node != NULL && value_node != NULL
                    ? span_between(key_node->span, value_node->span) : current_point(parser);
                const Om_Ast_Node_Id pair[2] = {key, value};
                status = make_node(parser, OM_AST_MAP_ENTRY, entry_span,
                                   (Om_String_View){NULL, 0}, OM_TOKEN_COLON,
                                   OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0,
                                   0, 0.0, pair, 2, &entry);
            }
            if (status == OM_STATUS_OK) {
                status = push_id(&entries, entry);
            }
            if (status != OM_STATUS_OK || !at_kind(parser, OM_TOKEN_COMMA)) {
                break;
            }
            (void)advance(parser);
            if (at_kind(parser, OM_TOKEN_RIGHT_BRACE)) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0204,
                                        peek(parser)->span, OM_TOKEN_IDENTIFIER,
                                        OM_KEYWORD_NONE);
                break;
            }
            if (parser->current == before) {
                (void)advance(parser);
            }
        }
        if (status != OM_STATUS_OK) {
            om_vector_destroy(&entries);
            return status;
        }
        const Om_Token *close = peek(parser);
        if (at_kind(parser, OM_TOKEN_RIGHT_BRACE)) {
            close = advance(parser);
        } else {
            status = expect_kind(parser, OM_TOKEN_RIGHT_BRACE);
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&entries);
                return status;
            }
        }
        status = make_node(parser, OM_AST_MAP, span_between(open->span, close->span),
                           (Om_String_View){NULL, 0}, OM_TOKEN_LEFT_BRACE,
                           OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                           (const Om_Ast_Node_Id *)entries.data,
                           entries.length, out);
        om_vector_destroy(&entries);
        return status;
    }
    Om_Status status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0204,
                                      token->span, OM_TOKEN_IDENTIFIER,
                                      OM_KEYWORD_NONE);
    if (status != OM_STATUS_OK) {
        return status;
    }
    return make_simple_node(parser, OM_AST_NAME, token->span, out);
}

static unsigned binary_precedence(const Om_Token *token)
{
    if (token->kind == OM_TOKEN_KEYWORD && token->keyword == OM_KEYWORD_OR) {
        return 1;
    }
    if (token->kind == OM_TOKEN_KEYWORD && token->keyword == OM_KEYWORD_AND) {
        return 2;
    }
    if (token->kind == OM_TOKEN_EQUAL || token->kind == OM_TOKEN_NOT_EQUAL
        || token->kind == OM_TOKEN_LESS || token->kind == OM_TOKEN_GREATER
        || token->kind == OM_TOKEN_LESS_EQUAL || token->kind == OM_TOKEN_GREATER_EQUAL) {
        return 3;
    }
    if (token->kind == OM_TOKEN_PLUS || token->kind == OM_TOKEN_MINUS) {
        return 4;
    }
    if (token->kind == OM_TOKEN_STAR || token->kind == OM_TOKEN_SLASH
        || token->kind == OM_TOKEN_PERCENT) {
        return 5;
    }
    return 0;
}

static bool is_comparison_operator(Om_Token_Kind kind)
{
    return kind == OM_TOKEN_EQUAL || kind == OM_TOKEN_NOT_EQUAL
           || kind == OM_TOKEN_LESS || kind == OM_TOKEN_GREATER
           || kind == OM_TOKEN_LESS_EQUAL || kind == OM_TOKEN_GREATER_EQUAL;
}

static Om_Status parse_postfix(Om_Parser *parser, Om_Ast_Node_Id *value)
{
    for (;;) {
        const Om_Ast_Node *base = node_at(parser, *value);
        if (base == NULL) {
            return OM_STATUS_OK;
        }
        const Om_Source_Span base_span = base->span;
        if (at_kind(parser, OM_TOKEN_LEFT_PAREN)) {
            (void)advance(parser);
            Om_Vector arguments = {0};
            Om_Status status = parse_expression_list(parser, &arguments,
                                                     OM_TOKEN_RIGHT_PAREN);
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&arguments);
                return status;
            }
            const Om_Token *close = peek(parser);
            if (at_kind(parser, OM_TOKEN_RIGHT_PAREN)) {
                close = advance(parser);
            } else {
                status = expect_kind(parser, OM_TOKEN_RIGHT_PAREN);
                if (status != OM_STATUS_OK) {
                    om_vector_destroy(&arguments);
                    return status;
                }
            }
            Om_Vector children = {0};
            status = init_id_list(&children);
            if (status == OM_STATUS_OK) {
                status = push_id(&children, *value);
            }
            if (status == OM_STATUS_OK) {
                status = push_list(&children, &arguments);
            }
            if (status == OM_STATUS_OK) {
                status = make_node(parser, OM_AST_CALL,
                                   span_between(base_span, close->span),
                                   (Om_String_View){NULL, 0}, OM_TOKEN_LEFT_PAREN,
                                   OM_KEYWORD_NONE, OM_AST_FLAG_NONE,
                                   (uint32_t)arguments.length, 0, 0.0,
                                   (const Om_Ast_Node_Id *)children.data,
                                   children.length, value);
            }
            om_vector_destroy(&children);
            om_vector_destroy(&arguments);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (at_kind(parser, OM_TOKEN_DOT)) {
            (void)advance(parser);
            Om_Token member = {0};
            const Om_Token *member_token = peek(parser);
            if (!is_name_token(member_token)) {
                return add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                                      member_token->span, OM_TOKEN_IDENTIFIER,
                                      OM_KEYWORD_NONE);
            }
            member = *advance(parser);
            Om_Status status = OM_STATUS_OK;
            const Om_Ast_Node_Id child = *value;
            status = make_node(parser, OM_AST_MEMBER,
                               span_between(base_span, member.span), member.lexeme,
                               OM_TOKEN_DOT, OM_KEYWORD_NONE,
                               flags_for_name(&member), 0, 0, 0.0, &child, 1, value);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (at_kind(parser, OM_TOKEN_LEFT_BRACKET)) {
            (void)advance(parser);
            Om_Ast_Node_Id index = OM_AST_NODE_ID_INVALID;
            Om_Status status = parse_expression(parser, 1, &index);
            if (status != OM_STATUS_OK) {
                return status;
            }
            const Om_Token *close = peek(parser);
            if (at_kind(parser, OM_TOKEN_RIGHT_BRACKET)) {
                close = advance(parser);
            } else {
                status = expect_kind(parser, OM_TOKEN_RIGHT_BRACKET);
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
            const Om_Ast_Node_Id children[2] = {*value, index};
            status = make_node(parser, OM_AST_INDEX,
                               span_between(base_span, close->span),
                               (Om_String_View){NULL, 0}, OM_TOKEN_LEFT_BRACKET,
                               OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                               children, 2, value);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (at_keyword(parser, OM_KEYWORD_WITH)) {
            (void)advance(parser);
            Om_Vector updates = {0};
            Om_Status status = init_id_list(&updates);
            if (status != OM_STATUS_OK) {
                return status;
            }
            for (;;) {
                Om_Token field = {0};
                status = expect_name(parser, false, &field);
                if (status != OM_STATUS_OK) {
                    om_vector_destroy(&updates);
                    return status;
                }
                if (at_kind(parser, OM_TOKEN_EQUAL)) {
                    (void)advance(parser);
                } else {
                    status = expect_kind(parser, OM_TOKEN_EQUAL);
                }
                Om_Ast_Node_Id expression = OM_AST_NODE_ID_INVALID;
                if (status == OM_STATUS_OK) {
                    status = parse_expression(parser, 1, &expression);
                }
                if (status != OM_STATUS_OK) {
                    om_vector_destroy(&updates);
                    return status;
                }
                const Om_Ast_Node *value_node = node_at(parser, expression);
                const Om_Source_Span update_span = value_node != NULL
                    ? span_between(field.span, value_node->span) : field.span;
                const Om_Ast_Node_Id child = expression;
                Om_Ast_Node_Id update = OM_AST_NODE_ID_INVALID;
                status = make_node(parser, OM_AST_FIELD_UPDATE, update_span,
                                   field.lexeme, field.kind, OM_KEYWORD_NONE,
                                   flags_for_name(&field), 0, 0, 0.0,
                                   &child, 1, &update);
                if (status == OM_STATUS_OK) {
                    status = push_id(&updates, update);
                }
                if (status != OM_STATUS_OK || !at_kind(parser, OM_TOKEN_COMMA)) {
                    break;
                }
                (void)advance(parser);
            }
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&updates);
                return status;
            }
            Om_Vector children = {0};
            status = init_id_list(&children);
            if (status == OM_STATUS_OK) {
                status = push_id(&children, *value);
            }
            if (status == OM_STATUS_OK) {
                status = push_list(&children, &updates);
            }
            const Om_Ast_Node *last_update = updates.length == 0
                ? NULL
                : node_at(parser, *(const Om_Ast_Node_Id *)om_vector_at_const(
                      &updates, updates.length - 1));
            if (status == OM_STATUS_OK) {
                status = make_node(parser, OM_AST_WITH,
                                   last_update != NULL
                                       ? span_between(base_span, last_update->span)
                                       : base_span,
                                   (Om_String_View){NULL, 0}, OM_TOKEN_ERROR,
                                   OM_KEYWORD_WITH, OM_AST_FLAG_NONE,
                                   (uint32_t)updates.length, 0, 0.0,
                                   (const Om_Ast_Node_Id *)children.data,
                                   children.length, value);
            }
            om_vector_destroy(&children);
            om_vector_destroy(&updates);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        return OM_STATUS_OK;
    }
}

static Om_Status parse_prefix(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *token = peek(parser);
    if (token->kind == OM_TOKEN_MINUS
        || (token->kind == OM_TOKEN_KEYWORD
            && (token->keyword == OM_KEYWORD_NOT || token->keyword == OM_KEYWORD_TRY))) {
        (void)advance(parser);
        Om_Ast_Node_Id operand = OM_AST_NODE_ID_INVALID;
        Om_Status status;
        if (token->kind == OM_TOKEN_MINUS && at_kind(parser, OM_TOKEN_INTEGER)
            && peek(parser)->integer_requires_negative) {
            status = parse_integer_node(parser, true, &operand);
        } else {
            status = parse_expression(parser, 6, &operand);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Ast_Node *operand_node = node_at(parser, operand);
        const Om_Source_Span span = operand_node != NULL
            ? span_between(token->span, operand_node->span) : token->span;
        const Om_Ast_Node_Id child = operand;
        return make_node(parser, OM_AST_UNARY, span, token->lexeme,
                         token->kind, token->keyword, OM_AST_FLAG_NONE, 0,
                         0, 0.0, &child, 1, out);
    }
    if (at_keyword(parser, OM_KEYWORD_SPAWN)) {
        (void)advance(parser);
        Om_Ast_Node_Id actor = OM_AST_NODE_ID_INVALID;
        Om_Status status = parse_primary(parser, &actor);
        if (status == OM_STATUS_OK) {
            status = parse_postfix(parser, &actor);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Ast_Node *actor_node = node_at(parser, actor);
        if (actor_node == NULL
            || (actor_node->kind != OM_AST_NAME
                && actor_node->kind != OM_AST_MEMBER)) {
            return add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                                  actor_node != NULL ? actor_node->span
                                                     : peek(parser)->span,
                                  OM_TOKEN_ENTITY_IDENTIFIER,
                                  OM_KEYWORD_NONE);
        }
        const Om_Ast_Node actor_value = *actor_node;
        Om_Ast_Node_Id placement = OM_AST_NODE_ID_INVALID;
        size_t child_count = 1;
        if (match_keyword(parser, OM_KEYWORD_AT)) {
            status = parse_primary(parser, &placement);
            if (status == OM_STATUS_OK) {
                status = parse_postfix(parser, &placement);
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
            child_count = 2;
        }
        const Om_Ast_Node *place_node = child_count != 0
            ? (child_count == 2 ? node_at(parser, placement) : NULL) : NULL;
        const Om_Source_Span span = place_node != NULL
            ? span_between(token->span, place_node->span)
            : span_between(token->span, actor_value.span);
        const Om_Ast_Node_Id children[2] = {actor, placement};
        return make_node(parser, OM_AST_SPAWN, span, actor_value.name,
                         actor_value.token_kind, OM_KEYWORD_SPAWN,
                         actor_value.flags, 0,
                         0, 0.0, children,
                         child_count, out);
    }
    Om_Status status = parse_primary(parser, out);
    if (status != OM_STATUS_OK) {
        return status;
    }
    return parse_postfix(parser, out);
}

static Om_Status parse_expression_impl(Om_Parser *parser, unsigned minimum_precedence,
                                       Om_Ast_Node_Id *out)
{
    Om_Status status = parse_prefix(parser, out);
    if (status != OM_STATUS_OK) {
        return status;
    }
    bool saw_comparison = false;
    for (;;) {
        const Om_Token *operator_token = peek(parser);
        const unsigned precedence = binary_precedence(operator_token);
        if (precedence == 0 || precedence < minimum_precedence) {
            return OM_STATUS_OK;
        }
        if (precedence == 3 && saw_comparison) {
            status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0206,
                                    operator_token->span, OM_TOKEN_ERROR,
                                    OM_KEYWORD_NONE);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        (void)advance(parser);
        Om_Ast_Node_Id right = OM_AST_NODE_ID_INVALID;
        status = parse_expression(parser, precedence + 1, &right);
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Ast_Node *left_node = node_at(parser, *out);
        const Om_Ast_Node *right_node = node_at(parser, right);
        const Om_Source_Span span = left_node != NULL && right_node != NULL
            ? span_between(left_node->span, right_node->span) : operator_token->span;
        const Om_Ast_Node_Id children[2] = {*out, right};
        status = make_node(parser, OM_AST_BINARY, span, operator_token->lexeme,
                           operator_token->kind, operator_token->keyword,
                           OM_AST_FLAG_NONE, 0, 0, 0.0, children, 2, out);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (is_comparison_operator(operator_token->kind)) {
            saw_comparison = true;
        }
    }
}

static Om_Status parse_expression(Om_Parser *parser, unsigned minimum_precedence,
                                  Om_Ast_Node_Id *out)
{
    if (parser->expression_depth >= 256) {
        Om_Status status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0208,
                                          peek(parser)->span, OM_TOKEN_ERROR,
                                          OM_KEYWORD_NONE);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (!at_kind(parser, OM_TOKEN_EOF) && !at_kind(parser, OM_TOKEN_NEWLINE)) {
            (void)advance(parser);
        }
        return make_simple_node(parser, OM_AST_NAME, current_point(parser), out);
    }
    ++parser->expression_depth;
    const Om_Status status = parse_expression_impl(parser, minimum_precedence, out);
    --parser->expression_depth;
    return status;
}

static Om_Status parse_query_expression(Om_Parser *parser,
                                        Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Token range_name = {0};
    Om_Status status = expect_name(parser, false, &range_name);
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_IN);
    }

    Om_Vector children = {0};
    status = status == OM_STATUS_OK ? init_id_list(&children) : status;
    Om_Ast_Node_Id source = OM_AST_NODE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = parse_expression(parser, 1, &source);
    }
    if (status == OM_STATUS_OK) {
        status = push_id(&children, source);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }

    bool saw_order = false;
    bool saw_take = false;
    bool saw_select = false;
    unsigned phase = 0;
    const Om_Token *end = begin;
    bool saw_end = false;
    while (status == OM_STATUS_OK && !at_kind(parser, OM_TOKEN_EOF)) {
        skip_newlines_and_docs(parser);
        if (at_kind(parser, OM_TOKEN_EOF)) {
            break;
        }
        if (at_keyword(parser, OM_KEYWORD_END)) {
            (void)advance(parser);
            end = peek(parser);
            status = expect_keyword(parser, OM_KEYWORD_QUERY);
            if (status == OM_STATUS_OK && !saw_select) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                    end->span, OM_TOKEN_KEYWORD, OM_KEYWORD_SELECT);
            }
            if (status == OM_STATUS_OK) {
                saw_end = true;
            }
            break;
        }
        if (at_keyword(parser, OM_KEYWORD_WHERE)) {
            const Om_Token *clause = advance(parser);
            if (phase > 0 || saw_select) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                    clause->span, OM_TOKEN_KEYWORD, OM_KEYWORD_ORDER);
            }
            Om_Ast_Node_Id expression = OM_AST_NODE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = parse_expression(parser, 1, &expression);
            }
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node_Id clause_children[] = {expression};
                Om_Ast_Node_Id clause_id = OM_AST_NODE_ID_INVALID;
                const Om_Ast_Node *expression_node = node_at(parser, expression);
                status = make_node(parser, OM_AST_QUERY_WHERE,
                    expression_node != NULL
                        ? span_between(clause->span, expression_node->span)
                        : clause->span,
                    (Om_String_View){NULL, 0}, OM_TOKEN_KEYWORD,
                    OM_KEYWORD_WHERE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                    clause_children, 1, &clause_id);
                if (status == OM_STATUS_OK) {
                    status = push_id(&children, clause_id);
                }
            }
            if (status == OM_STATUS_OK) {
                status = consume_line_end(parser);
            }
            continue;
        }
        if (at_keyword(parser, OM_KEYWORD_ORDER)) {
            const Om_Token *clause = advance(parser);
            if (phase > 1 || saw_order || saw_select) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                    clause->span, OM_TOKEN_KEYWORD, OM_KEYWORD_TAKE);
            }
            if (status == OM_STATUS_OK) {
                status = expect_keyword(parser, OM_KEYWORD_BY);
            }
            if (status == OM_STATUS_OK) {
                phase = 1;
                saw_order = true;
            }
            skip_newlines_and_docs(parser);
            Om_Vector keys = {0};
            if (status == OM_STATUS_OK) {
                status = init_id_list(&keys);
            }
            while (status == OM_STATUS_OK
                   && !at_kind(parser, OM_TOKEN_EOF)
                   && !at_kind(parser, OM_TOKEN_NEWLINE)
                   && !at_keyword(parser, OM_KEYWORD_END)) {
                Om_Ast_Node_Id key_expression = OM_AST_NODE_ID_INVALID;
                status = parse_expression(parser, 1, &key_expression);
                if (status != OM_STATUS_OK) {
                    break;
                }
                uint32_t key_flags = OM_AST_FLAG_NONE;
                if (match_keyword(parser, OM_KEYWORD_ASCENDING)) {
                    /* Ascending is the default; the explicit token is retained
                       by the source span and is not otherwise semantically used. */
                } else if (match_keyword(parser, OM_KEYWORD_DESCENDING)) {
                    key_flags |= OM_AST_FLAG_QUERY_DESCENDING;
                } else {
                    status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                        peek(parser)->span, OM_TOKEN_KEYWORD,
                        OM_KEYWORD_ASCENDING);
                }
                const Om_Ast_Node *key_node = node_at(parser, key_expression);
                Om_Ast_Node_Id key_id = OM_AST_NODE_ID_INVALID;
                if (status == OM_STATUS_OK) {
                    status = make_node(parser, OM_AST_QUERY_ORDER_KEY,
                        key_node != NULL ? key_node->span : clause->span,
                        (Om_String_View){NULL, 0}, OM_TOKEN_KEYWORD,
                        OM_KEYWORD_ORDER, key_flags, 0, 0, 0.0,
                        &key_expression, 1, &key_id);
                }
                if (status == OM_STATUS_OK) {
                    status = push_id(&keys, key_id);
                }
                if (status != OM_STATUS_OK || !at_kind(parser, OM_TOKEN_COMMA)) {
                    break;
                }
                (void)advance(parser);
                skip_newlines_and_docs(parser);
            }
            if (status == OM_STATUS_OK && keys.length == 0) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                    peek(parser)->span, OM_TOKEN_IDENTIFIER, OM_KEYWORD_NONE);
            }
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node_Id *key_ids = keys.data;
                const Om_Ast_Node *last_key = keys.length != 0
                    ? node_at(parser, key_ids[keys.length - 1]) : NULL;
                Om_Ast_Node_Id order_id = OM_AST_NODE_ID_INVALID;
                status = make_node(parser, OM_AST_QUERY_ORDER,
                    last_key != NULL
                        ? span_between(clause->span, last_key->span)
                        : clause->span,
                    (Om_String_View){NULL, 0}, OM_TOKEN_KEYWORD,
                    OM_KEYWORD_ORDER, OM_AST_FLAG_NONE, 0, 0, 0.0,
                    key_ids, keys.length, &order_id);
                if (status == OM_STATUS_OK) {
                    status = push_id(&children, order_id);
                }
            }
            om_vector_destroy(&keys);
            if (status == OM_STATUS_OK) {
                status = consume_line_end(parser);
            }
            continue;
        }
        if (at_keyword(parser, OM_KEYWORD_TAKE)) {
            const Om_Token *clause = advance(parser);
            if (phase > 2 || saw_take || saw_select) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                    clause->span, OM_TOKEN_KEYWORD, OM_KEYWORD_SELECT);
            }
            if (status == OM_STATUS_OK) {
                phase = 2;
                saw_take = true;
            }
            Om_Ast_Node_Id expression = OM_AST_NODE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = parse_expression(parser, 1, &expression);
            }
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node_Id clause_children[] = {expression};
                const Om_Ast_Node *expression_node = node_at(parser, expression);
                Om_Ast_Node_Id clause_id = OM_AST_NODE_ID_INVALID;
                status = make_node(parser, OM_AST_QUERY_TAKE,
                    expression_node != NULL
                        ? span_between(clause->span, expression_node->span)
                        : clause->span,
                    (Om_String_View){NULL, 0}, OM_TOKEN_KEYWORD,
                    OM_KEYWORD_TAKE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                    clause_children, 1, &clause_id);
                if (status == OM_STATUS_OK) {
                    status = push_id(&children, clause_id);
                }
            }
            if (status == OM_STATUS_OK) {
                status = consume_line_end(parser);
            }
            continue;
        }
        if (at_keyword(parser, OM_KEYWORD_SELECT)) {
            const Om_Token *clause = advance(parser);
            if (phase > 3 || saw_select) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                    clause->span, OM_TOKEN_KEYWORD, OM_KEYWORD_END);
            }
            phase = 3;
            saw_select = true;
            Om_Ast_Node_Id expression = OM_AST_NODE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = parse_expression(parser, 1, &expression);
            }
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node_Id clause_children[] = {expression};
                const Om_Ast_Node *expression_node = node_at(parser, expression);
                Om_Ast_Node_Id clause_id = OM_AST_NODE_ID_INVALID;
                status = make_node(parser, OM_AST_QUERY_SELECT,
                    expression_node != NULL
                        ? span_between(clause->span, expression_node->span)
                        : clause->span,
                    (Om_String_View){NULL, 0}, OM_TOKEN_KEYWORD,
                    OM_KEYWORD_SELECT, OM_AST_FLAG_NONE, 0, 0, 0.0,
                    clause_children, 1, &clause_id);
                if (status == OM_STATUS_OK) {
                    status = push_id(&children, clause_id);
                }
            }
            if (status == OM_STATUS_OK) {
                status = consume_line_end(parser);
            }
            continue;
        }

        status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
            peek(parser)->span, OM_TOKEN_KEYWORD, OM_KEYWORD_END);
        if (status == OM_STATUS_OK) {
            while (!at_kind(parser, OM_TOKEN_NEWLINE)
                   && !at_kind(parser, OM_TOKEN_EOF)
                   && !at_keyword(parser, OM_KEYWORD_END)) {
                (void)advance(parser);
            }
            if (at_kind(parser, OM_TOKEN_NEWLINE)) {
                (void)advance(parser);
            }
        }
    }
    if (status == OM_STATUS_OK && !saw_end) {
        status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
            peek(parser)->span, OM_TOKEN_KEYWORD, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        const Om_Ast_Node_Id *query_children = children.data;
        status = make_node(parser, OM_AST_QUERY,
            span_between(begin->span, end->span), range_name.lexeme,
            OM_TOKEN_KEYWORD, OM_KEYWORD_QUERY, OM_AST_FLAG_NONE, 0, 0, 0.0,
            query_children, children.length, out);
    }
    om_vector_destroy(&children);
    return status;
}

static Om_Status parse_parameter_list(Om_Parser *parser, Om_Vector *parameters)
{
    Om_Status status = init_id_list(parameters);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (at_kind(parser, OM_TOKEN_LEFT_PAREN)) {
        (void)advance(parser);
    } else {
        status = expect_kind(parser, OM_TOKEN_LEFT_PAREN);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    while (!at_kind(parser, OM_TOKEN_RIGHT_PAREN) && !at_kind(parser, OM_TOKEN_EOF)) {
        const size_t before = parser->current;
        Om_Token parameter = {0};
        status = expect_name(parser, false, &parameter);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (parameter.lexeme.data != NULL || parameter.lexeme.length != 0) {
            Om_Ast_Node_Id parameter_node = OM_AST_NODE_ID_INVALID;
            status = make_parameter(parser, &parameter, &parameter_node);
            if (status == OM_STATUS_OK) {
                status = push_id(parameters, parameter_node);
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        if (at_kind(parser, OM_TOKEN_COMMA)) {
            (void)advance(parser);
            if (at_kind(parser, OM_TOKEN_RIGHT_PAREN)) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                                        peek(parser)->span, OM_TOKEN_IDENTIFIER,
                                        OM_KEYWORD_NONE);
                if (status != OM_STATUS_OK) {
                    return status;
                }
                break;
            }
            continue;
        }
        if (!at_kind(parser, OM_TOKEN_RIGHT_PAREN)) {
            status = expect_kind(parser, OM_TOKEN_COMMA);
            if (status != OM_STATUS_OK) {
                return status;
            }
            if (parser->current == before) {
                break;
            }
        }
        if (parser->current == before) {
            break;
        }
    }
    return expect_kind(parser, OM_TOKEN_RIGHT_PAREN);
}

static Om_Status parse_block_impl(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Source_Span start = current_point(parser);
    Om_Vector statements = {0};
    Om_Status status = init_id_list(&statements);
    if (status != OM_STATUS_OK) {
        return status;
    }
    while (!is_block_terminator(parser)) {
        skip_newlines_and_docs(parser);
        if (is_block_terminator(parser)) {
            break;
        }
        const size_t before = parser->current;
        Om_Ast_Node_Id statement = OM_AST_NODE_ID_INVALID;
        status = parse_statement(parser, &statement);
        if (status == OM_STATUS_OK && statement.value != UINT32_MAX) {
            status = push_id(&statements, statement);
        }
        if (status != OM_STATUS_OK) {
            om_vector_destroy(&statements);
            return status;
        }
        if (parser->current == before && !at_kind(parser, OM_TOKEN_EOF)) {
            status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0208,
                                    peek(parser)->span, OM_TOKEN_ERROR,
                                    OM_KEYWORD_NONE);
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&statements);
                return status;
            }
            (void)advance(parser);
        }
    }
    const Om_Source_Span end = parser->current > 0
        ? parser->lexed->tokens[parser->current - 1].span : start;
    status = make_node(parser, OM_AST_BLOCK,
                       statements.length == 0 ? start : span_between(start, end),
                       (Om_String_View){NULL, 0}, OM_TOKEN_ERROR,
                       OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                       (const Om_Ast_Node_Id *)statements.data,
                       statements.length, out);
    om_vector_destroy(&statements);
    return status;
}

static Om_Status parse_block(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    if (parser->block_depth >= 256) {
        Om_Status status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0208,
                                          peek(parser)->span, OM_TOKEN_ERROR,
                                          OM_KEYWORD_NONE);
        if (status != OM_STATUS_OK) {
            return status;
        }
        return make_simple_node(parser, OM_AST_BLOCK, current_point(parser), out);
    }
    ++parser->block_depth;
    const Om_Status status = parse_block_impl(parser, out);
    --parser->block_depth;
    return status;
}

static Om_Status parse_if_statement(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Vector arms = {0};
    Om_Status status = init_id_list(&arms);
    if (status != OM_STATUS_OK) {
        return status;
    }
    bool has_else = false;
    Om_Ast_Node_Id else_block = OM_AST_NODE_ID_INVALID;
    for (;;) {
        Om_Ast_Node_Id condition = OM_AST_NODE_ID_INVALID;
        status = parse_expression(parser, 1, &condition);
        if (status == OM_STATUS_OK) {
            status = expect_keyword(parser, OM_KEYWORD_THEN);
        }
        if (status == OM_STATUS_OK) {
            status = consume_line_end(parser);
        }
        if (status != OM_STATUS_OK) {
            om_vector_destroy(&arms);
            return status;
        }
        Om_Ast_Node_Id body = OM_AST_NODE_ID_INVALID;
        status = parse_block(parser, &body);
        if (status != OM_STATUS_OK) {
            om_vector_destroy(&arms);
            return status;
        }
        const Om_Ast_Node *condition_node = node_at(parser, condition);
        const Om_Ast_Node *body_node = node_at(parser, body);
        const Om_Source_Span arm_span = condition_node != NULL && body_node != NULL
            ? span_between(condition_node->span, body_node->span) : begin->span;
        const Om_Ast_Node_Id arm_children[2] = {condition, body};
        Om_Ast_Node_Id arm = OM_AST_NODE_ID_INVALID;
        status = make_node(parser, OM_AST_IF_ARM, arm_span,
                           (Om_String_View){NULL, 0}, OM_TOKEN_ERROR,
                           OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                           arm_children, 2, &arm);
        if (status == OM_STATUS_OK) {
            status = push_id(&arms, arm);
        }
        if (status != OM_STATUS_OK) {
            om_vector_destroy(&arms);
            return status;
        }
        if (!at_keyword(parser, OM_KEYWORD_ELSE)) {
            break;
        }
        (void)advance(parser);
        if (match_keyword(parser, OM_KEYWORD_IF)) {
            continue;
        }
        has_else = true;
        status = consume_line_end(parser);
        if (status == OM_STATUS_OK) {
            status = parse_block(parser, &else_block);
        }
        break;
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_IF);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Vector children = {0};
    if (status == OM_STATUS_OK) {
        status = init_id_list(&children);
    }
    if (status == OM_STATUS_OK) {
        status = push_list(&children, &arms);
    }
    if (status == OM_STATUS_OK && has_else) {
        status = push_id(&children, else_block);
    }
    if (status == OM_STATUS_OK) {
        const Om_Source_Span end = parser->current > 0
            ? parser->lexed->tokens[parser->current - 1].span : begin->span;
        status = make_node(parser, OM_AST_IF, span_between(begin->span, end),
                           (Om_String_View){NULL, 0}, begin->kind,
                           begin->keyword,
                           has_else ? OM_AST_FLAG_HAS_ELSE : OM_AST_FLAG_NONE,
                           (uint32_t)arms.length, 0, 0.0,
                           (const Om_Ast_Node_Id *)children.data,
                           children.length, out);
    }
    om_vector_destroy(&children);
    om_vector_destroy(&arms);
    return status;
}

static Om_Status parse_while_statement(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Ast_Node_Id condition = OM_AST_NODE_ID_INVALID;
    Om_Status status = parse_expression(parser, 1, &condition);
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Ast_Node_Id body = OM_AST_NODE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = parse_block(parser, &body);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_WHILE);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Ast_Node *body_node = node_at(parser, body);
    const Om_Source_Span end = parser->current > 0
        ? parser->lexed->tokens[parser->current - 1].span : begin->span;
    const Om_Ast_Node_Id children[2] = {condition, body};
    (void)body_node;
    return make_node(parser, OM_AST_WHILE, span_between(begin->span, end),
                     (Om_String_View){NULL, 0}, begin->kind, begin->keyword,
                     OM_AST_FLAG_NONE, 0, 0, 0.0, children, 2, out);
}

static Om_Status parse_for_statement(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Token variable = {0};
    Om_Status status = expect_name(parser, false, &variable);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Vector children = {0};
    status = init_id_list(&children);
    if (status != OM_STATUS_OK) {
        return status;
    }
    uint32_t flags = OM_AST_FLAG_NONE;
    if (at_kind(parser, OM_TOKEN_EQUAL)) {
        flags = OM_AST_FLAG_FOR_NUMERIC;
        (void)advance(parser);
        Om_Ast_Node_Id start = OM_AST_NODE_ID_INVALID;
        Om_Ast_Node_Id end = OM_AST_NODE_ID_INVALID;
        Om_Ast_Node_Id step = OM_AST_NODE_ID_INVALID;
        status = parse_expression(parser, 1, &start);
        if (status == OM_STATUS_OK) {
            status = expect_keyword(parser, OM_KEYWORD_TO);
        }
        if (status == OM_STATUS_OK) {
            status = parse_expression(parser, 1, &end);
        }
        if (status == OM_STATUS_OK) {
            status = push_id(&children, start);
        }
        if (status == OM_STATUS_OK) {
            status = push_id(&children, end);
        }
        if (status == OM_STATUS_OK && match_keyword(parser, OM_KEYWORD_STEP)) {
            status = parse_expression(parser, 1, &step);
            if (status == OM_STATUS_OK) {
                status = push_id(&children, step);
            }
        }
    } else if (match_keyword(parser, OM_KEYWORD_IN)) {
        flags = OM_AST_FLAG_FOR_EACH;
        Om_Ast_Node_Id iterable = OM_AST_NODE_ID_INVALID;
        status = parse_expression(parser, 1, &iterable);
        if (status == OM_STATUS_OK) {
            status = push_id(&children, iterable);
        }
    } else {
        status = expect_keyword(parser, OM_KEYWORD_IN);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Ast_Node_Id body = OM_AST_NODE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = parse_block(parser, &body);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_FOR);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status == OM_STATUS_OK) {
        status = push_id(&children, body);
    }
    if (status == OM_STATUS_OK) {
        const Om_Source_Span end = parser->current > 0
            ? parser->lexed->tokens[parser->current - 1].span : begin->span;
        status = make_node(parser, OM_AST_FOR, span_between(begin->span, end),
                           variable.lexeme, variable.kind, begin->keyword,
                           flags | flags_for_name(&variable), 0, 0, 0.0,
                           (const Om_Ast_Node_Id *)children.data,
                           children.length, out);
    }
    om_vector_destroy(&children);
    return status;
}

static Om_Status parse_select_statement(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Ast_Node_Id scrutinee = OM_AST_NODE_ID_INVALID;
    Om_Status status = parse_expression(parser, 1, &scrutinee);
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Vector arms = {0};
    if (status == OM_STATUS_OK) {
        status = init_id_list(&arms);
    }
    bool had_else = false;
    while (status == OM_STATUS_OK && at_keyword(parser, OM_KEYWORD_CASE)) {
        const Om_Token *case_token = advance(parser);
        bool case_else = match_keyword(parser, OM_KEYWORD_ELSE);
        if (had_else) {
            status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0208,
                                    case_token->span, OM_TOKEN_ERROR,
                                    OM_KEYWORD_NONE);
        }
        if (!case_else && status == OM_STATUS_OK && had_else) {
            had_else = true;
        }
        Om_Ast_Node_Id pattern = OM_AST_NODE_ID_INVALID;
        if (!case_else && status == OM_STATUS_OK) {
            status = parse_expression(parser, 1, &pattern);
        }
        if (status == OM_STATUS_OK) {
            status = consume_line_end(parser);
        }
        Om_Ast_Node_Id body = OM_AST_NODE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = parse_block(parser, &body);
        }
        if (status == OM_STATUS_OK) {
            const Om_Ast_Node *body_node = node_at(parser, body);
            const Om_Ast_Node *pattern_node = case_else ? NULL : node_at(parser, pattern);
            const Om_Source_Span arm_span = pattern_node != NULL && body_node != NULL
                ? span_between(case_token->span, body_node->span)
                : body_node != NULL ? span_between(case_token->span, body_node->span)
                                    : case_token->span;
            Om_Ast_Node_Id case_children[2] = {pattern, body};
            status = make_node(parser, OM_AST_CASE_ARM, arm_span,
                               (Om_String_View){NULL, 0}, case_token->kind,
                               case_token->keyword,
                               case_else ? OM_AST_FLAG_CASE_ELSE : OM_AST_FLAG_NONE,
                               0, 0, 0.0, case_else ? &case_children[1] : case_children,
                               case_else ? 1 : 2, &pattern);
            if (status == OM_STATUS_OK) {
                status = push_id(&arms, pattern);
            }
        }
        if (case_else) {
            had_else = true;
        }
        skip_newlines_and_docs(parser);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_SELECT);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Vector children = {0};
    if (status == OM_STATUS_OK) {
        status = init_id_list(&children);
    }
    if (status == OM_STATUS_OK) {
        status = push_id(&children, scrutinee);
    }
    if (status == OM_STATUS_OK) {
        status = push_list(&children, &arms);
    }
    if (status == OM_STATUS_OK) {
        const Om_Source_Span end = parser->current > 0
            ? parser->lexed->tokens[parser->current - 1].span : begin->span;
        status = make_node(parser, OM_AST_SELECT, span_between(begin->span, end),
                           (Om_String_View){NULL, 0}, begin->kind, begin->keyword,
                           OM_AST_FLAG_NONE, 0, 0, 0.0,
                           (const Om_Ast_Node_Id *)children.data,
                           children.length, out);
    }
    om_vector_destroy(&children);
    om_vector_destroy(&arms);
    return status;
}

static bool is_call_expression(const Om_Parser *parser, Om_Ast_Node_Id id)
{
    const Om_Ast_Node *node = node_at(parser, id);
    while (node != NULL) {
        if (node->kind == OM_AST_CALL) {
            return true;
        }
        if ((node->kind == OM_AST_MEMBER || node->kind == OM_AST_INDEX
             || node->kind == OM_AST_WITH) && node->child_count > 0) {
            const Om_Ast_Node_Id *children = parser->children.data;
            id = children[node->first_child];
            node = node_at(parser, id);
        } else {
            return false;
        }
    }
    return false;
}

static Om_Status parse_statement(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *token = peek(parser);
    if (at_keyword(parser, OM_KEYWORD_IF)) {
        return parse_if_statement(parser, out);
    }
    if (at_keyword(parser, OM_KEYWORD_WHILE)) {
        return parse_while_statement(parser, out);
    }
    if (at_keyword(parser, OM_KEYWORD_FOR)) {
        return parse_for_statement(parser, out);
    }
    if (at_keyword(parser, OM_KEYWORD_SELECT)) {
        return parse_select_statement(parser, out);
    }
    if ((at_keyword(parser, OM_KEYWORD_VAR)
         && (is_name_token(peek_next(parser)) || peek_next(parser)->kind == OM_TOKEN_KEYWORD))
        || (is_name_token(token) && peek_next(parser)->kind == OM_TOKEN_EQUAL)) {
        bool mutable_binding = match_keyword(parser, OM_KEYWORD_VAR);
        Om_Token name = {0};
        Om_Status status = expect_name(parser, false, &name);
        if (status == OM_STATUS_OK) {
            status = expect_kind(parser, OM_TOKEN_EQUAL);
        }
        Om_Ast_Node_Id value = OM_AST_NODE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = parse_expression(parser, 1, &value);
        }
        if (status == OM_STATUS_OK) {
            status = consume_line_end(parser);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Ast_Node *value_node = node_at(parser, value);
        const Om_Source_Span span = value_node != NULL
            ? span_between(name.span, value_node->span) : name.span;
        const Om_Ast_Node_Id child = value;
        return make_node(parser, OM_AST_BINDING, span, name.lexeme,
                         name.kind, OM_KEYWORD_NONE,
                         flags_for_name(&name)
                             | (mutable_binding ? OM_AST_FLAG_MUTABLE : OM_AST_FLAG_NONE),
                         0, 0, 0.0, &child, 1, out);
    }
    if (match_keyword(parser, OM_KEYWORD_RETURN)) {
        Om_Ast_Node_Id value = OM_AST_NODE_ID_INVALID;
        size_t child_count = 0;
        Om_Status status = OM_STATUS_OK;
        if (!at_kind(parser, OM_TOKEN_NEWLINE) && !at_kind(parser, OM_TOKEN_EOF)
            && !is_block_terminator(parser)) {
            status = parse_expression(parser, 1, &value);
            child_count = 1;
        }
        if (status == OM_STATUS_OK) {
            status = consume_line_end(parser);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Source_Span end = child_count != 0 && node_at(parser, value) != NULL
            ? node_at(parser, value)->span : token->span;
        return make_node(parser, OM_AST_RETURN, span_between(token->span, end),
                         (Om_String_View){NULL, 0}, token->kind,
                         token->keyword, OM_AST_FLAG_NONE, 0, 0, 0.0,
                         child_count != 0 ? &value : NULL, child_count, out);
    }
    if (match_keyword(parser, OM_KEYWORD_BREAK)
        || match_keyword(parser, OM_KEYWORD_CONTINUE)
        || match_keyword(parser, OM_KEYWORD_STOP)) {
        const Om_Keyword keyword = token->keyword;
        Om_Ast_Kind kind = OM_AST_BREAK;
        if (keyword == OM_KEYWORD_CONTINUE) {
            kind = OM_AST_CONTINUE;
        } else if (keyword == OM_KEYWORD_STOP) {
            kind = OM_AST_STOP;
        }
        Om_Status status = consume_line_end(parser);
        if (status != OM_STATUS_OK) {
            return status;
        }
        return make_node(parser, kind, token->span, (Om_String_View){NULL, 0},
                         token->kind, keyword, OM_AST_FLAG_NONE, 0,
                         0, 0.0, NULL, 0, out);
    }
    if (match_keyword(parser, OM_KEYWORD_SEND)) {
        Om_Ast_Node_Id receiver = OM_AST_NODE_ID_INVALID;
        Om_Ast_Node_Id message = OM_AST_NODE_ID_INVALID;
        Om_Status status = parse_expression(parser, 1, &receiver);
        if (status == OM_STATUS_OK) {
            status = expect_kind(parser, OM_TOKEN_COMMA);
        }
        if (status == OM_STATUS_OK) {
            status = parse_expression(parser, 1, &message);
        }
        if (status == OM_STATUS_OK) {
            status = consume_line_end(parser);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Ast_Node *message_node = node_at(parser, message);
        const Om_Source_Span span = message_node != NULL
            ? span_between(token->span, message_node->span) : token->span;
        const Om_Ast_Node_Id children[2] = {receiver, message};
        return make_node(parser, OM_AST_SEND, span, (Om_String_View){NULL, 0},
                         token->kind, token->keyword, OM_AST_FLAG_NONE, 0,
                         0, 0.0, children, 2, out);
    }
    if (match_keyword(parser, OM_KEYWORD_WATCH)) {
        Om_Ast_Node_Id actor = OM_AST_NODE_ID_INVALID;
        Om_Status status = parse_expression(parser, 1, &actor);
        if (status == OM_STATUS_OK) {
            status = consume_line_end(parser);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Ast_Node *actor_node = node_at(parser, actor);
        const Om_Source_Span span = actor_node != NULL
            ? span_between(token->span, actor_node->span) : token->span;
        return make_node(parser, OM_AST_WATCH, span, (Om_String_View){NULL, 0},
                         token->kind, token->keyword, OM_AST_FLAG_NONE, 0,
                         0, 0.0, &actor, 1, out);
    }
    if (!at_kind(parser, OM_TOKEN_EOF) && !at_kind(parser, OM_TOKEN_NEWLINE)
        && !is_block_terminator(parser)) {
        Om_Ast_Node_Id expression = OM_AST_NODE_ID_INVALID;
        Om_Status status = parse_expression(parser, 1, &expression);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (!is_call_expression(parser, expression)) {
            status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0207,
                                    token->span, OM_TOKEN_LEFT_PAREN,
                                    OM_KEYWORD_NONE);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        if (status == OM_STATUS_OK) {
            status = consume_line_end(parser);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Ast_Node *expression_node = node_at(parser, expression);
        const Om_Source_Span span = expression_node != NULL
            ? expression_node->span : token->span;
        return make_node(parser, OM_AST_CALL_STATEMENT, span,
                         (Om_String_View){NULL, 0}, token->kind,
                         OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                         &expression, 1, out);
    }
    Om_Status status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
                                      token->span, OM_TOKEN_IDENTIFIER,
                                      OM_KEYWORD_NONE);
    if (status == OM_STATUS_OK) {
        *out = OM_AST_NODE_ID_INVALID;
    }
    return status;
}

static Om_Status parse_import(Om_Parser *parser, bool reexport,
                              const Om_Token *prefix,
                              Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    const Om_Token *path = peek(parser);
    Om_Status status = expect_kind(parser, OM_TOKEN_STRING);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = consume_line_end(parser);
    if (status != OM_STATUS_OK) {
        return status;
    }
    return make_node(parser, OM_AST_IMPORT,
                     path->kind == OM_TOKEN_STRING
                         ? span_between(prefix != NULL ? prefix->span : begin->span,
                                        path->span)
                         : (prefix != NULL ? prefix->span : begin->span),
                     path->kind == OM_TOKEN_STRING ? path->lexeme
                                                   : (Om_String_View){NULL, 0},
                     OM_TOKEN_STRING, OM_KEYWORD_IMPORT,
                     reexport ? OM_AST_FLAG_REEXPORT : OM_AST_FLAG_NONE, 0,
                     0, 0.0, NULL, 0, out);
}

static Om_Status parse_native_import(Om_Parser *parser,
                                    Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Status status = expect_keyword(parser, OM_KEYWORD_IMPORT);
    const Om_Token *path = peek(parser);
    if (status == OM_STATUS_OK) {
        status = expect_kind(parser, OM_TOKEN_STRING);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    return make_node(parser, OM_AST_NATIVE_IMPORT,
                     span_between(begin->span, path->span), path->lexeme,
                     OM_TOKEN_STRING, OM_KEYWORD_NATIVE, OM_AST_FLAG_NONE,
                     0, 0, 0.0, NULL, 0, out);
}

static Om_Status parse_simple_declaration(Om_Parser *parser, bool exported,
                                          Om_Ast_Kind kind, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    const bool entity = kind == OM_AST_RECORD_DECLARATION
                        || kind == OM_AST_MESSAGE_DECLARATION
                        || kind == OM_AST_ACTOR_DECLARATION;
    Om_Token name = {0};
    Om_Status status = expect_name(parser, entity, &name);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Vector parameters = {0};
    status = parse_parameter_list(parser, &parameters);
    const Om_Source_Span declaration_end = parser->current > 0
        ? parser->lexed->tokens[parser->current - 1].span : name.span;
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status == OM_STATUS_OK) {
        status = make_node(parser, kind,
                           name.lexeme.data != NULL
                               ? span_between(begin->span, declaration_end) : begin->span,
                           name.lexeme, name.kind, begin->keyword,
                           flags_for_name(&name)
                               | (exported ? OM_AST_FLAG_EXPORTED : OM_AST_FLAG_NONE),
                           (uint32_t)parameters.length, 0, 0.0,
                           (const Om_Ast_Node_Id *)parameters.data,
                           parameters.length, out);
    }
    om_vector_destroy(&parameters);
    return status;
}

static Om_Status parse_callable_declaration(Om_Parser *parser, bool exported,
                                            Om_Ast_Kind kind, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Token name = {0};
    Om_Status status = expect_name(parser, false, &name);
    Om_Vector parameters = {0};
    if (status == OM_STATUS_OK) {
        status = parse_parameter_list(parser, &parameters);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Ast_Node_Id body = OM_AST_NODE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = parse_block(parser, &body);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, kind == OM_AST_FUNCTION_DECLARATION
                                           ? OM_KEYWORD_FUNCTION : OM_KEYWORD_ACTION);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status == OM_STATUS_OK) {
        status = push_id(&parameters, body);
    }
    if (status == OM_STATUS_OK) {
        const Om_Source_Span end = parser->current > 0
            ? parser->lexed->tokens[parser->current - 1].span : begin->span;
        status = make_node(parser, kind, span_between(begin->span, end),
                           name.lexeme, name.kind, begin->keyword,
                           flags_for_name(&name)
                               | (exported ? OM_AST_FLAG_EXPORTED : OM_AST_FLAG_NONE),
                           (uint32_t)(parameters.length - 1), 0, 0.0,
                           (const Om_Ast_Node_Id *)parameters.data,
                           parameters.length, out);
    }
    om_vector_destroy(&parameters);
    return status;
}

static Om_Status parse_actor_state(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Token name = {0};
    Om_Status status = expect_name(parser, false, &name);
    if (status == OM_STATUS_OK) {
        status = expect_kind(parser, OM_TOKEN_EQUAL);
    }
    Om_Ast_Node_Id value = OM_AST_NODE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = parse_expression(parser, 1, &value);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Ast_Node *value_node = node_at(parser, value);
    const Om_Source_Span span = value_node != NULL
        ? span_between(begin->span, value_node->span) : begin->span;
    return make_node(parser, OM_AST_BINDING, span, name.lexeme,
                     name.kind, begin->keyword,
                     flags_for_name(&name) | OM_AST_FLAG_MUTABLE,
                     0, 0, 0.0, &value, 1, out);
}

static Om_Status parse_receive(Om_Parser *parser, Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Token message = {0};
    const Om_Token *first_name = peek(parser);
    Om_Status status = OM_STATUS_OK;
    if (!is_name_token(first_name)) {
        status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0202,
            first_name->span, OM_TOKEN_ENTITY_IDENTIFIER, OM_KEYWORD_NONE);
    } else {
        message = *advance(parser);
    }
    if (status == OM_STATUS_OK && at_kind(parser, OM_TOKEN_DOT)) {
        (void)advance(parser);
        Om_Token member = {0};
        status = expect_name(parser, true, &member);
        if (status == OM_STATUS_OK) {
            message.lexeme.length += 1 + member.lexeme.length;
            message.kind = member.kind;
            message.span = span_between(message.span, member.span);
        }
    } else if (status == OM_STATUS_OK
               && message.kind != OM_TOKEN_ENTITY_IDENTIFIER
               && message.kind != OM_TOKEN_INVALID_IDENTIFIER) {
        status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0102,
            message.span, OM_TOKEN_INVALID_IDENTIFIER, OM_KEYWORD_NONE);
    }
    Om_Vector parameters = {0};
    if (status == OM_STATUS_OK) {
        status = parse_parameter_list(parser, &parameters);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Ast_Node_Id body = OM_AST_NODE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = parse_block(parser, &body);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_RECEIVE);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status == OM_STATUS_OK) {
        status = push_id(&parameters, body);
    }
    if (status == OM_STATUS_OK) {
        const Om_Source_Span end = parser->current > 0
            ? parser->lexed->tokens[parser->current - 1].span : begin->span;
        status = make_node(parser, OM_AST_RECEIVE_DECLARATION,
                           span_between(begin->span, end), message.lexeme,
                           message.kind, begin->keyword,
                           flags_for_name(&message),
                           (uint32_t)(parameters.length - 1), 0, 0.0,
                           (const Om_Ast_Node_Id *)parameters.data,
                           parameters.length, out);
    }
    om_vector_destroy(&parameters);
    return status;
}

static Om_Status parse_actor_declaration(Om_Parser *parser, bool exported,
                                         Om_Ast_Node_Id *out)
{
    const Om_Token *begin = advance(parser);
    Om_Token name = {0};
    Om_Status status = expect_name(parser, true, &name);
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    Om_Vector members = {0};
    if (status == OM_STATUS_OK) {
        status = init_id_list(&members);
    }
    bool receiving_started = false;
    while (status == OM_STATUS_OK && !at_keyword(parser, OM_KEYWORD_END)
           && !at_kind(parser, OM_TOKEN_EOF)) {
        skip_newlines_and_docs(parser);
        if (at_keyword(parser, OM_KEYWORD_END) || at_kind(parser, OM_TOKEN_EOF)) {
            break;
        }
        const size_t before = parser->current;
        Om_Ast_Node_Id member = OM_AST_NODE_ID_INVALID;
        if (at_keyword(parser, OM_KEYWORD_VAR)) {
            if (receiving_started) {
                status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0208,
                                        peek(parser)->span, OM_TOKEN_ERROR,
                                        OM_KEYWORD_NONE);
            }
            if (status == OM_STATUS_OK) {
                status = parse_actor_state(parser, &member);
            }
        } else if (at_keyword(parser, OM_KEYWORD_RECEIVE)) {
            receiving_started = true;
            status = parse_receive(parser, &member);
        } else {
            status = add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0203,
                                    peek(parser)->span, OM_TOKEN_ERROR,
                                    OM_KEYWORD_NONE);
            if (status == OM_STATUS_OK) {
                while (!at_kind(parser, OM_TOKEN_NEWLINE)
                       && !at_kind(parser, OM_TOKEN_EOF)
                       && !at_keyword(parser, OM_KEYWORD_END)) {
                    (void)advance(parser);
                }
                status = consume_line_end(parser);
            }
        }
        if (status == OM_STATUS_OK && member.value != UINT32_MAX) {
            status = push_id(&members, member);
        }
        if (status != OM_STATUS_OK) {
            break;
        }
        if (parser->current == before && !at_kind(parser, OM_TOKEN_EOF)) {
            (void)advance(parser);
        }
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_END);
    }
    if (status == OM_STATUS_OK) {
        status = expect_keyword(parser, OM_KEYWORD_ACTOR);
    }
    if (status == OM_STATUS_OK) {
        status = consume_line_end(parser);
    }
    if (status == OM_STATUS_OK) {
        const Om_Source_Span end = parser->current > 0
            ? parser->lexed->tokens[parser->current - 1].span : begin->span;
        status = make_node(parser, OM_AST_ACTOR_DECLARATION,
                           span_between(begin->span, end), name.lexeme,
                           name.kind, begin->keyword,
                           flags_for_name(&name)
                               | (exported ? OM_AST_FLAG_EXPORTED : OM_AST_FLAG_NONE),
                           0, 0, 0.0,
                           (const Om_Ast_Node_Id *)members.data,
                           members.length, out);
    }
    om_vector_destroy(&members);
    return status;
}

static Om_Status parse_declaration(Om_Parser *parser, bool exported,
                                   Om_Ast_Node_Id *out)
{
    if (at_keyword(parser, OM_KEYWORD_RECORD)) {
        return parse_simple_declaration(parser, exported,
                                        OM_AST_RECORD_DECLARATION, out);
    }
    if (at_keyword(parser, OM_KEYWORD_MESSAGE)) {
        return parse_simple_declaration(parser, exported,
                                        OM_AST_MESSAGE_DECLARATION, out);
    }
    if (at_keyword(parser, OM_KEYWORD_FUNCTION)) {
        return parse_callable_declaration(parser, exported,
                                          OM_AST_FUNCTION_DECLARATION, out);
    }
    if (at_keyword(parser, OM_KEYWORD_ACTION)) {
        return parse_callable_declaration(parser, exported,
                                          OM_AST_ACTION_DECLARATION, out);
    }
    if (at_keyword(parser, OM_KEYWORD_ACTOR)) {
        return parse_actor_declaration(parser, exported, out);
    }
    return add_diagnostic(parser, OM_PARSE_DIAGNOSTIC_E0203,
                          peek(parser)->span, OM_TOKEN_KEYWORD,
                          OM_KEYWORD_NONE);
}

void om_ast_destroy(Om_Ast *ast)
{
    if (ast == NULL) {
        return;
    }
    om_free(ast->owned_nodes);
    om_free(ast->owned_children);
    *ast = (Om_Ast){.root = OM_AST_NODE_ID_INVALID};
}

const Om_Ast_Node *om_ast_get_node(const Om_Ast *ast, Om_Ast_Node_Id id)
{
    if (ast == NULL || id.value >= ast->node_count) {
        return NULL;
    }
    return &ast->nodes[id.value];
}

Om_Ast_Node_Id om_ast_get_child(const Om_Ast *ast, const Om_Ast_Node *node,
                                size_t index)
{
    if (ast == NULL || node == NULL || index >= node->child_count
        || node->first_child > ast->child_count
        || node->first_child == ast->child_count
        || index >= ast->child_count - node->first_child) {
        return OM_AST_NODE_ID_INVALID;
    }
    return ast->children[node->first_child + (uint32_t)index];
}

void om_parser_result_destroy(Om_Parser_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_ast_destroy(&result->ast);
    om_free(result->diagnostics);
    *result = (Om_Parser_Result){.ast = {.root = OM_AST_NODE_ID_INVALID}};
}

Om_Status om_parser_parse(const Om_Lexer_Result *lexed, Om_Parser_Result *result)
{
    Om_Parser parser = {0};
    Om_Status status = OM_STATUS_OK;
    if (lexed == NULL || result == NULL || lexed->tokens == NULL
        || lexed->token_count == 0 || result->ast.nodes != NULL
        || result->ast.children != NULL || result->diagnostics != NULL
        || lexed->tokens[lexed->token_count - 1].kind != OM_TOKEN_EOF) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index + 1 < lexed->token_count; ++index) {
        if (lexed->tokens[index].kind == OM_TOKEN_EOF) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    parser.lexed = lexed;
    status = om_vector_init(&parser.nodes, sizeof(Om_Ast_Node));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser.children, sizeof(Om_Ast_Node_Id));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser.diagnostics, sizeof(Om_Parse_Diagnostic));
    }
    Om_Vector top_level = {0};
    if (status == OM_STATUS_OK) {
        status = init_id_list(&top_level);
    }
    bool imports_open = true;
    while (status == OM_STATUS_OK && !at_kind(&parser, OM_TOKEN_EOF)) {
        skip_newlines_and_docs(&parser);
        if (at_kind(&parser, OM_TOKEN_EOF)) {
            break;
        }
        const size_t before = parser.current;
        if (at_keyword(&parser, OM_KEYWORD_IMPORT)) {
            if (!imports_open) {
                status = add_diagnostic(&parser, OM_PARSE_DIAGNOSTIC_E0203,
                                        peek(&parser)->span, OM_TOKEN_ERROR,
                                        OM_KEYWORD_NONE);
            }
            Om_Ast_Node_Id import_node = OM_AST_NODE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = parse_import(&parser, false, NULL, &import_node);
            }
            if (status == OM_STATUS_OK) {
                status = push_id(&top_level, import_node);
            }
        } else if (at_keyword(&parser, OM_KEYWORD_NATIVE)
                   && peek_next(&parser)->kind == OM_TOKEN_KEYWORD
                   && peek_next(&parser)->keyword == OM_KEYWORD_IMPORT) {
            if (!imports_open) {
                status = add_diagnostic(&parser, OM_PARSE_DIAGNOSTIC_E0203,
                                        peek(&parser)->span, OM_TOKEN_ERROR,
                                        OM_KEYWORD_NONE);
            }
            Om_Ast_Node_Id import_node = OM_AST_NODE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = parse_native_import(&parser, &import_node);
            }
            if (status == OM_STATUS_OK) {
                status = push_id(&top_level, import_node);
            }
        } else {
            const Om_Token *export_prefix = at_keyword(&parser, OM_KEYWORD_EXPORT)
                ? peek(&parser) : NULL;
            bool exported = match_keyword(&parser, OM_KEYWORD_EXPORT);
            if (exported && at_keyword(&parser, OM_KEYWORD_IMPORT)) {
                if (!imports_open) {
                    status = add_diagnostic(&parser, OM_PARSE_DIAGNOSTIC_E0203,
                                            peek(&parser)->span, OM_TOKEN_ERROR,
                                            OM_KEYWORD_NONE);
                }
                Om_Ast_Node_Id import_node = OM_AST_NODE_ID_INVALID;
                if (status == OM_STATUS_OK) {
                    status = parse_import(&parser, true, export_prefix,
                                         &import_node);
                }
                if (status == OM_STATUS_OK) {
                    status = push_id(&top_level, import_node);
                }
                continue;
            }
            const bool declaration_start = at_keyword(&parser, OM_KEYWORD_RECORD)
                || at_keyword(&parser, OM_KEYWORD_MESSAGE)
                || at_keyword(&parser, OM_KEYWORD_FUNCTION)
                || at_keyword(&parser, OM_KEYWORD_ACTION)
                || at_keyword(&parser, OM_KEYWORD_ACTOR);
            if (declaration_start) {
                imports_open = false;
                Om_Ast_Node_Id declaration = OM_AST_NODE_ID_INVALID;
                status = parse_declaration(&parser, exported, &declaration);
                if (status == OM_STATUS_OK) {
                    status = push_id(&top_level, declaration);
                }
            } else {
                status = add_diagnostic(&parser, OM_PARSE_DIAGNOSTIC_E0203,
                                        peek(&parser)->span,
                                        OM_TOKEN_KEYWORD, OM_KEYWORD_NONE);
                while (status == OM_STATUS_OK
                       && !at_kind(&parser, OM_TOKEN_NEWLINE)
                       && !at_kind(&parser, OM_TOKEN_EOF)) {
                    (void)advance(&parser);
                }
                if (status == OM_STATUS_OK && at_kind(&parser, OM_TOKEN_NEWLINE)) {
                    status = consume_line_end(&parser);
                }
            }
        }
        if (parser.current == before && !at_kind(&parser, OM_TOKEN_EOF)) {
            (void)advance(&parser);
        }
    }
    if (status == OM_STATUS_OK) {
        const Om_Token *first = &lexed->tokens[0];
        const Om_Token *last = &lexed->tokens[lexed->token_count - 1];
        const Om_Source_Span program_span = span_between(first->span, last->span);
        status = make_node(&parser, OM_AST_PROGRAM, program_span,
                           (Om_String_View){NULL, 0}, OM_TOKEN_EOF,
                           OM_KEYWORD_NONE, OM_AST_FLAG_NONE, 0, 0, 0.0,
                           (const Om_Ast_Node_Id *)top_level.data,
                           top_level.length, &result->ast.root);
    }
    om_vector_destroy(&top_level);
    if (status != OM_STATUS_OK) {
        om_vector_destroy(&parser.nodes);
        om_vector_destroy(&parser.children);
        om_vector_destroy(&parser.diagnostics);
        result->ast.root = OM_AST_NODE_ID_INVALID;
        return status;
    }
    result->ast.nodes = parser.nodes.data;
    result->ast.node_count = parser.nodes.length;
    result->ast.children = parser.children.data;
    result->ast.child_count = parser.children.length;
    result->ast.owned_nodes = parser.nodes.data;
    result->ast.owned_children = parser.children.data;
    result->diagnostics = parser.diagnostics.data;
    result->diagnostic_count = parser.diagnostics.length;
    parser.nodes = (Om_Vector){0};
    parser.children = (Om_Vector){0};
    parser.diagnostics = (Om_Vector){0};
    return OM_STATUS_OK;
}

const char *om_parse_diagnostic_code_name(Om_Parse_Diagnostic_Code code)
{
    switch (code) {
    case OM_PARSE_DIAGNOSTIC_E0102: return "E0102";
    case OM_PARSE_DIAGNOSTIC_E0202: return "E0202";
    case OM_PARSE_DIAGNOSTIC_E0203: return "E0203";
    case OM_PARSE_DIAGNOSTIC_E0204: return "E0204";
    case OM_PARSE_DIAGNOSTIC_E0205: return "E0205";
    case OM_PARSE_DIAGNOSTIC_E0206: return "E0206";
    case OM_PARSE_DIAGNOSTIC_E0207: return "E0207";
    case OM_PARSE_DIAGNOSTIC_E0208: return "E0208";
    default: return "E0000";
    }
}
