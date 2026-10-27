#include "ombasic/tooling/formatter.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

typedef struct Om_Format_Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Om_Format_Buffer;

static Om_Status reserve(Om_Format_Buffer *buffer, size_t extra)
{
    if (buffer == NULL || extra > SIZE_MAX - buffer->length - 1) {
        return OM_STATUS_OVERFLOW;
    }
    const size_t required = buffer->length + extra + 1;
    if (required <= buffer->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *data = om_realloc_array(buffer->data, capacity, sizeof(*data));
    if (data == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

static Om_Status append_bytes(Om_Format_Buffer *buffer, const char *bytes,
                              size_t length)
{
    if (length != 0 && bytes == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = reserve(buffer, length);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (length != 0) {
        (void)memcpy(buffer->data + buffer->length, bytes, length);
        buffer->length += length;
    }
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

static Om_Status append_char(Om_Format_Buffer *buffer, char value)
{
    return append_bytes(buffer, &value, 1);
}

static Om_Status append_indent(Om_Format_Buffer *buffer, size_t indentation)
{
    static const char spaces[] = "                                                                ";
    while (indentation != 0) {
        const size_t amount = indentation > sizeof(spaces) - 1
            ? sizeof(spaces) - 1 : indentation;
        Om_Status status = append_bytes(buffer, spaces, amount);
        if (status != OM_STATUS_OK) {
            return status;
        }
        indentation -= amount;
    }
    return OM_STATUS_OK;
}

static bool is_word_kind(Om_Token_Kind kind)
{
    return kind == OM_TOKEN_IDENTIFIER || kind == OM_TOKEN_ENTITY_IDENTIFIER
           || kind == OM_TOKEN_INVALID_IDENTIFIER || kind == OM_TOKEN_KEYWORD
           || kind == OM_TOKEN_INTEGER || kind == OM_TOKEN_FLOAT
           || kind == OM_TOKEN_STRING || kind == OM_TOKEN_STRING_TEXT;
}

static bool is_binary_operator(const Om_Token *token)
{
    if (token == NULL) {
        return false;
    }
    switch (token->kind) {
    case OM_TOKEN_STAR:
    case OM_TOKEN_SLASH:
    case OM_TOKEN_PERCENT:
    case OM_TOKEN_EQUAL:
    case OM_TOKEN_NOT_EQUAL:
    case OM_TOKEN_LESS:
    case OM_TOKEN_GREATER:
    case OM_TOKEN_LESS_EQUAL:
    case OM_TOKEN_GREATER_EQUAL:
        return true;
    case OM_TOKEN_PLUS:
    case OM_TOKEN_MINUS:
        return true;
    case OM_TOKEN_KEYWORD:
        return token->keyword == OM_KEYWORD_AND || token->keyword == OM_KEYWORD_OR;
    default:
        return false;
    }
}

static bool is_expression_prefix(const Om_Token *previous)
{
    if (previous == NULL) {
        return true;
    }
    if (previous->kind == OM_TOKEN_LEFT_PAREN
        || previous->kind == OM_TOKEN_LEFT_BRACKET
        || previous->kind == OM_TOKEN_LEFT_BRACE
        || previous->kind == OM_TOKEN_COMMA
        || previous->kind == OM_TOKEN_COLON
        || is_binary_operator(previous)) {
        return true;
    }
    return previous->kind == OM_TOKEN_KEYWORD
           && previous->keyword != OM_KEYWORD_TRUE
           && previous->keyword != OM_KEYWORD_FALSE
           && previous->keyword != OM_KEYWORD_NULL
           && previous->keyword != OM_KEYWORD_SELF
           && previous->keyword != OM_KEYWORD_NORMAL
           && previous->keyword != OM_KEYWORD_FAILED
           && previous->keyword != OM_KEYWORD_NODE_UNREACHABLE;
}

static bool unary_operator_at(const Om_Lexer_Result *lexed, size_t index)
{
    const Om_Token *token = &lexed->tokens[index];
    if (token->kind != OM_TOKEN_PLUS && token->kind != OM_TOKEN_MINUS) {
        return false;
    }
    const Om_Token *previous = index == 0 ? NULL : &lexed->tokens[index - 1];
    return is_expression_prefix(previous);
}

static bool is_no_space_before(const Om_Token *current)
{
    if (current == NULL) {
        return false;
    }
    switch (current->kind) {
    case OM_TOKEN_DOT:
    case OM_TOKEN_COMMA:
    case OM_TOKEN_COLON:
    case OM_TOKEN_RIGHT_PAREN:
    case OM_TOKEN_RIGHT_BRACKET:
    case OM_TOKEN_RIGHT_BRACE:
    case OM_TOKEN_INTERPOLATION_END:
    case OM_TOKEN_INTERPOLATED_STRING_END:
        return true;
    default:
        return false;
    }
}

static bool is_binary_operator_at(const Om_Lexer_Result *lexed, size_t index)
{
    const Om_Token *token = &lexed->tokens[index];
    return is_binary_operator(token) && !unary_operator_at(lexed, index);
}

static bool needs_space_before(const Om_Lexer_Result *lexed, size_t index)
{
    if (index == 0) {
        return false;
    }
    const Om_Token *previous = &lexed->tokens[index - 1];
    const Om_Token *current = &lexed->tokens[index];
    if (previous->kind == OM_TOKEN_NEWLINE || previous->kind == OM_TOKEN_DOC_COMMENT
        || current->kind == OM_TOKEN_DOC_COMMENT) {
        return false;
    }
    if (is_no_space_before(current)) {
        return false;
    }
    if (current->kind == OM_TOKEN_LEFT_PAREN) {
        if (previous->kind == OM_TOKEN_KEYWORD) {
            return true;
        }
        return false;
    }
    if (current->kind == OM_TOKEN_LEFT_BRACKET
        || current->kind == OM_TOKEN_LEFT_BRACE
        || current->kind == OM_TOKEN_INTERPOLATION_START) {
        return false;
    }
    if (current->kind == OM_TOKEN_DOT) {
        return false;
    }
    if (current->kind == OM_TOKEN_COMMA || current->kind == OM_TOKEN_COLON) {
        return false;
    }
    if (current->kind == OM_TOKEN_PLUS || current->kind == OM_TOKEN_MINUS) {
        return true;
    }
    if (is_binary_operator_at(lexed, index)) {
        return true;
    }
    if (is_binary_operator_at(lexed, index - 1)) {
        return true;
    }
    if (previous->kind == OM_TOKEN_COMMA || previous->kind == OM_TOKEN_COLON) {
        return true;
    }
    if (previous->kind == OM_TOKEN_KEYWORD
        && previous->keyword == OM_KEYWORD_NOT) {
        return true;
    }
    if (previous->kind == OM_TOKEN_KEYWORD
        && previous->keyword == OM_KEYWORD_TRY) {
        return true;
    }
    if (is_word_kind(previous->kind) && is_word_kind(current->kind)) {
        return true;
    }
    if (previous->kind == OM_TOKEN_INTERPOLATION_START
        || previous->kind == OM_TOKEN_INTERPOLATED_STRING_START) {
        return false;
    }
    return false;
}

static bool is_line_terminator(const Om_Token *token)
{
    return token != NULL && token->kind == OM_TOKEN_KEYWORD
           && token->keyword == OM_KEYWORD_END;
}

static bool is_clause_start(const Om_Token *token)
{
    return token != NULL && token->kind == OM_TOKEN_KEYWORD
           && (token->keyword == OM_KEYWORD_ELSE || token->keyword == OM_KEYWORD_CASE);
}

static bool line_opens_block(const Om_Token *first, const Om_Token *second)
{
    const Om_Token *head = first;
    if (head != NULL && head->kind == OM_TOKEN_KEYWORD
        && head->keyword == OM_KEYWORD_EXPORT) {
        head = second;
    }
    return head != NULL && head->kind == OM_TOKEN_KEYWORD
           && (head->keyword == OM_KEYWORD_FUNCTION
               || head->keyword == OM_KEYWORD_ACTION
               || head->keyword == OM_KEYWORD_ACTOR
               || head->keyword == OM_KEYWORD_RECEIVE
               || head->keyword == OM_KEYWORD_IF
               || head->keyword == OM_KEYWORD_WHILE
               || head->keyword == OM_KEYWORD_FOR
               || head->keyword == OM_KEYWORD_SELECT);
}

static Om_Status append_gap_comment(Om_Format_Buffer *output,
                                    const Om_Source_Text *source,
                                    uint32_t start, uint32_t end,
                                    size_t indentation, bool *line_started,
                                    bool *found_comment)
{
    *found_comment = false;
    if (end < start || end > source->bytes.length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t *gap = source->bytes.data + start;
    const size_t gap_length = (size_t)(end - start);
    size_t comment_index = 0;
    while (comment_index < gap_length && gap[comment_index] != (uint8_t)'\'') {
        ++comment_index;
    }
    if (comment_index == gap_length) {
        return OM_STATUS_OK;
    }
    Om_Status status = OM_STATUS_OK;
    if (!*line_started) {
        status = append_indent(output, indentation * 4);
    } else {
        status = append_char(output, ' ');
    }
    if (status == OM_STATUS_OK) {
        status = append_bytes(output, (const char *)gap + comment_index,
                              gap_length - comment_index);
    }
    if (status == OM_STATUS_OK) {
        *line_started = true;
        *found_comment = true;
    }
    return status;
}

Om_Status om_formatter_format(
    const Om_Source_Text *source,
    const Om_Lexer_Result *lexed,
    const Om_Parser_Result *parsed,
    Om_Formatter_Result *result
)
{
    Om_Format_Buffer output = {0};
    Om_Status status = OM_STATUS_OK;
    size_t indentation = 0;
    size_t consecutive_newlines = 0;
    bool line_started = false;
    bool current_line_opens = false;
    const Om_Token *line_first = NULL;
    const Om_Token *line_second = NULL;
    uint32_t trivia_cursor = 0;

    if (source == NULL || lexed == NULL || parsed == NULL || result == NULL
        || result->data != NULL || lexed->tokens == NULL
        || lexed->diagnostic_count != 0 || parsed->diagnostic_count != 0
        || parsed->ast.nodes == NULL || parsed->ast.root.value >= parsed->ast.node_count
        || lexed->token_count == 0
        || lexed->tokens[lexed->token_count - 1].kind != OM_TOKEN_EOF) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    for (size_t index = 0; status == OM_STATUS_OK && index < lexed->token_count; ++index) {
        const Om_Token *token = &lexed->tokens[index];
        if (token->span.start < trivia_cursor || token->span.end < token->span.start
            || token->span.end > source->bytes.length) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        bool found_comment = false;
        status = append_gap_comment(&output, source, trivia_cursor, token->span.start,
                                    indentation, &line_started, &found_comment);
        if (status != OM_STATUS_OK) {
            break;
        }
        if (found_comment && line_first == NULL) {
            line_first = token;
            current_line_opens = false;
        }
        if (token->kind == OM_TOKEN_EOF) {
            trivia_cursor = token->span.end;
            break;
        }
        if (token->kind == OM_TOKEN_NEWLINE) {
            if (line_started) {
                status = append_char(&output, '\n');
                if (status == OM_STATUS_OK) {
                    ++consecutive_newlines;
                }
            } else if (consecutive_newlines < 2) {
                status = append_char(&output, '\n');
                if (status == OM_STATUS_OK) {
                    ++consecutive_newlines;
                }
            }
            if (status == OM_STATUS_OK && current_line_opens
                && indentation < SIZE_MAX / 4) {
                ++indentation;
            }
            line_started = false;
            current_line_opens = false;
            line_first = NULL;
            line_second = NULL;
            trivia_cursor = token->span.end;
            continue;
        }

        const bool line_start = !line_started;
        if (line_start) {
            if (is_line_terminator(token) && indentation != 0) {
                --indentation;
            }
            size_t display_indent = indentation;
            if (is_clause_start(token) && display_indent != 0) {
                --display_indent;
            }
            status = append_indent(&output, display_indent * 4);
            if (status != OM_STATUS_OK) {
                break;
            }
            line_started = true;
            consecutive_newlines = 0;
            line_first = token;
            current_line_opens = false;
        } else if (needs_space_before(lexed, index)) {
            if (output.length != 0 && output.data[output.length - 1] != ' '
                && output.data[output.length - 1] != '\n') {
                status = append_char(&output, ' ');
            }
        }
        if (status != OM_STATUS_OK) {
            break;
        }
        if (line_first != token && line_second == NULL) {
            line_second = token;
            current_line_opens = line_opens_block(line_first, line_second);
        }
        if (token->kind == OM_TOKEN_DOC_COMMENT) {
            status = append_bytes(&output, token->lexeme.data, token->lexeme.length);
        } else {
            status = append_bytes(&output, token->lexeme.data, token->lexeme.length);
        }
        trivia_cursor = token->span.end;
    }
    if (status == OM_STATUS_OK && line_started) {
        status = append_char(&output, '\n');
    }
    if (status == OM_STATUS_OK) {
        if (output.data == NULL) {
            output.data = om_alloc(1);
            if (output.data == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                output.data[0] = '\0';
            }
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(output.data);
        return status;
    }
    result->data = output.data;
    result->length = output.length;
    return OM_STATUS_OK;
}

void om_formatter_result_destroy(Om_Formatter_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->data);
    *result = (Om_Formatter_Result){0};
}
