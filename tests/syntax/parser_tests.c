#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/source/source_text.h"
#include "ombasic/syntax/lexer.h"
#include "ombasic/syntax/parser.h"

#define PARSER_ASSERT(expression)                                                \
    do {                                                                          \
        if (!(expression)) {                                                      \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",              \
                          __FILE__, __LINE__, #expression);                       \
            goto cleanup;                                                        \
        }                                                                         \
    } while (false)

static bool scan_and_parse(Om_Source_Text *source, Om_Lexer_Result *lexed,
                           Om_Parser_Result *parsed, const uint8_t *bytes,
                           size_t length, uint32_t source_id)
{
    static const char path[] = "parser_test.bas";
    if (om_source_text_init(source, (Om_Source_Id){source_id},
                            (Om_String_View){path, sizeof(path) - 1},
                            (Om_Const_Byte_Slice){bytes, length}, 0) != OM_STATUS_OK) {
        return false;
    }
    if (om_lexer_scan(source, lexed) != OM_STATUS_OK) {
        return false;
    }
    return om_parser_parse(lexed, parsed) == OM_STATUS_OK;
}

static const char *ast_kind_name(Om_Ast_Kind kind)
{
    switch (kind) {
    case OM_AST_PROGRAM: return "Program";
    case OM_AST_IMPORT: return "Import";
    case OM_AST_NATIVE_IMPORT: return "NativeImport";
    case OM_AST_RECORD_DECLARATION: return "RecordDeclaration";
    case OM_AST_MESSAGE_DECLARATION: return "MessageDeclaration";
    case OM_AST_FUNCTION_DECLARATION: return "FunctionDeclaration";
    case OM_AST_ACTION_DECLARATION: return "ActionDeclaration";
    case OM_AST_ACTOR_DECLARATION: return "ActorDeclaration";
    case OM_AST_RECEIVE_DECLARATION: return "ReceiveDeclaration";
    case OM_AST_PARAMETER: return "Parameter";
    case OM_AST_BLOCK: return "Block";
    case OM_AST_BINDING: return "Binding";
    case OM_AST_RETURN: return "Return";
    case OM_AST_BREAK: return "Break";
    case OM_AST_CONTINUE: return "Continue";
    case OM_AST_SEND: return "Send";
    case OM_AST_WATCH: return "Watch";
    case OM_AST_STOP: return "Stop";
    case OM_AST_CALL_STATEMENT: return "CallStatement";
    case OM_AST_IF: return "If";
    case OM_AST_IF_ARM: return "IfArm";
    case OM_AST_WHILE: return "While";
    case OM_AST_FOR: return "For";
    case OM_AST_SELECT: return "Select";
    case OM_AST_CASE_ARM: return "CaseArm";
    case OM_AST_NAME: return "Name";
    case OM_AST_INTEGER_LITERAL: return "Integer";
    case OM_AST_FLOAT_LITERAL: return "Float";
    case OM_AST_STRING_LITERAL: return "String";
    case OM_AST_STRING_TEXT: return "StringText";
    case OM_AST_INTERPOLATED_STRING: return "InterpolatedString";
    case OM_AST_BOOLEAN_LITERAL: return "Boolean";
    case OM_AST_NULL_LITERAL: return "Null";
    case OM_AST_SELF: return "Self";
    case OM_AST_UNARY: return "Unary";
    case OM_AST_BINARY: return "Binary";
    case OM_AST_CALL: return "Call";
    case OM_AST_MEMBER: return "Member";
    case OM_AST_INDEX: return "Index";
    case OM_AST_WITH: return "With";
    case OM_AST_FIELD_UPDATE: return "FieldUpdate";
    case OM_AST_SPAWN: return "Spawn";
    case OM_AST_GROUP: return "Group";
    case OM_AST_LIST: return "List";
    case OM_AST_MAP: return "Map";
    case OM_AST_MAP_ENTRY: return "MapEntry";
    case OM_AST_QUERY: return "Query";
    case OM_AST_QUERY_WHERE: return "QueryWhere";
    case OM_AST_QUERY_ORDER: return "QueryOrder";
    case OM_AST_QUERY_ORDER_KEY: return "QueryOrderKey";
    case OM_AST_QUERY_TAKE: return "QueryTake";
    case OM_AST_QUERY_SELECT: return "QuerySelect";
    default: return "Unknown";
    }
}

static const Om_Ast_Node *ast_child_node(const Om_Ast *ast,
                                         const Om_Ast_Node *node,
                                         size_t index)
{
    return node != NULL
        ? om_ast_get_node(ast, om_ast_get_child(ast, node, index)) : NULL;
}

static bool append_bytes(char *output, size_t capacity, size_t *length,
                         const char *bytes, size_t count)
{
    if (*length > capacity || count >= capacity - *length) {
        return false;
    }
    (void)memcpy(output + *length, bytes, count);
    *length += count;
    output[*length] = '\0';
    return true;
}

static bool append_node(const Om_Ast *ast, Om_Ast_Node_Id id, char *output,
                        size_t capacity, size_t *length, unsigned depth)
{
    if (depth > 512) {
        return false;
    }
    const Om_Ast_Node *node = om_ast_get_node(ast, id);
    if (node == NULL) {
        return false;
    }
    const char *kind = ast_kind_name(node->kind);
    if (!append_bytes(output, capacity, length, kind, strlen(kind))) {
        return false;
    }
    if (node->name.data != NULL && node->name.length != 0) {
        if (!append_bytes(output, capacity, length, ":", 1)
            || !append_bytes(output, capacity, length, node->name.data,
                             node->name.length)) {
            return false;
        }
    } else if (node->kind == OM_AST_BINARY || node->kind == OM_AST_UNARY) {
        if (!append_bytes(output, capacity, length, ":", 1)
            || !append_bytes(output, capacity, length, node->name.data,
                             node->name.length)) {
            return false;
        }
    }
    if (node->child_count == 0) {
        return true;
    }
    if (!append_bytes(output, capacity, length, "(", 1)) {
        return false;
    }
    for (size_t index = 0; index < node->child_count; ++index) {
        if (index != 0 && !append_bytes(output, capacity, length, ",", 1)) {
            return false;
        }
        if (!append_node(ast, om_ast_get_child(ast, node, index), output,
                         capacity, length, depth + 1)) {
            return false;
        }
    }
    return append_bytes(output, capacity, length, ")", 1);
}

static bool read_file(const char *path, uint8_t **bytes, size_t *length)
{
    FILE *file = NULL;
#if defined(_MSC_VER)
    if (fopen_s(&file, path, "rb") != 0) {
        return false;
    }
#else
    file = fopen(path, "rb");
#endif
    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return false;
    }
    const long file_length = ftell(file);
    if (file_length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    const size_t count = (size_t)file_length;
    uint8_t *data = count == 0 ? NULL : om_alloc(count);
    if (count != 0 && data == NULL) {
        (void)fclose(file);
        return false;
    }
    const size_t read_count = count == 0 ? 0 : fread(data, 1, count, file);
    const bool closed = fclose(file) == 0;
    const bool ok = read_count == count && closed;
    if (!ok) {
        om_free(data);
        return false;
    }
    *bytes = data;
    *length = count;
    return true;
}

static size_t count_kind(const Om_Ast *ast, Om_Ast_Kind kind)
{
    size_t count = 0;
    for (size_t index = 0; index < ast->node_count; ++index) {
        if (ast->nodes[index].kind == kind) {
            ++count;
        }
    }
    return count;
}

static bool ast_invariants_hold(const Om_Source_Text *source,
                                const Om_Parser_Result *parsed)
{
    const Om_Ast *ast = &parsed->ast;
    if (om_ast_get_node(ast, ast->root) == NULL || ast->root.value >= ast->node_count) {
        return false;
    }
    for (size_t index = 0; index < ast->node_count; ++index) {
        const Om_Ast_Node *node = &ast->nodes[index];
        if (node->span.source.value != source->id.value
            || node->span.start > node->span.end
            || node->span.end > source->bytes.length
            || node->first_child > ast->child_count
            || node->child_count > ast->child_count - node->first_child) {
            return false;
        }
        for (size_t child = 0; child < node->child_count; ++child) {
            const Om_Ast_Node_Id child_id = om_ast_get_child(ast, node, child);
            if (om_ast_get_node(ast, child_id) == NULL) {
                return false;
            }
        }
    }
    for (size_t index = 0; index < parsed->diagnostic_count; ++index) {
        const Om_Source_Span span = parsed->diagnostics[index].span;
        if (span.source.value != source->id.value || span.start > span.end
            || span.end > source->bytes.length) {
            return false;
        }
    }
    return true;
}

bool om_test_parser_ast_golden(void)
{
    char source_path[1024] = {0};
    char golden_path[1024] = {0};
    uint8_t *source_bytes = NULL;
    size_t source_length = 0;
    uint8_t *golden_bytes = NULL;
    size_t golden_length = 0;
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    PARSER_ASSERT(snprintf(source_path, sizeof(source_path), "%s/basic.bas",
                           OM_PARSER_GOLDEN_ROOT) > 0);
    PARSER_ASSERT(snprintf(golden_path, sizeof(golden_path), "%s/basic.ast",
                           OM_PARSER_GOLDEN_ROOT) > 0);
    PARSER_ASSERT(read_file(source_path, &source_bytes, &source_length));
    PARSER_ASSERT(read_file(golden_path, &golden_bytes, &golden_length));
    PARSER_ASSERT(om_source_text_init(
        &source, (Om_Source_Id){101}, (Om_String_View){"golden.bas", 10},
        (Om_Const_Byte_Slice){source_bytes, source_length}, 0
    ) == OM_STATUS_OK);
    PARSER_ASSERT(om_lexer_scan(&source, &lexed) == OM_STATUS_OK);
    PARSER_ASSERT(lexed.diagnostic_count == 0);
    PARSER_ASSERT(om_parser_parse(&lexed, &parsed) == OM_STATUS_OK);
    PARSER_ASSERT(parsed.diagnostic_count == 0);
    PARSER_ASSERT(ast_invariants_hold(&source, &parsed));
    char normalized[8192] = {0};
    size_t normalized_length = 0;
    PARSER_ASSERT(append_node(&parsed.ast, parsed.ast.root, normalized,
                              sizeof(normalized), &normalized_length, 0));
    while (golden_length != 0
           && (golden_bytes[golden_length - 1] == (uint8_t)'\n'
               || golden_bytes[golden_length - 1] == (uint8_t)'\r')) {
        --golden_length;
    }
    PARSER_ASSERT(normalized_length == golden_length);
    PARSER_ASSERT(memcmp(normalized, golden_bytes, golden_length) == 0);

    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    om_free(source_bytes);
    om_free(golden_bytes);
    return true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    om_free(source_bytes);
    om_free(golden_bytes);
    return false;
}

bool om_test_parser_language_grammar(void)
{
    static const uint8_t input[] =
        "IMPORT \"dep.bas\"\n"
        "EXPORT RECORD User(name, age)\n"
        "MESSAGE Done(value)\n"
        "FUNCTION calculate(value)\n"
        "    VAR result = -9223372036854775808\n"
        "    IF value > 0 THEN\n"
        "        result = value + 1\n"
        "    ELSE IF value = 0 THEN\n"
        "        result = TRY lookup(value)\n"
        "    ELSE\n"
        "        result = 0\n"
        "    END IF\n"
        "    WHILE result < 4\n"
        "        IF result = 2 THEN\n"
        "            CONTINUE\n"
        "        END IF\n"
        "        result = result + 1\n"
        "    END WHILE\n"
        "    FOR index = 1 TO 4 STEP -1\n"
        "        print(index)\n"
        "    END FOR\n"
        "    FOR item IN [1, 2]\n"
        "        print(item)\n"
        "    END FOR\n"
        "    VAR settings = {\"x\": 1, \"y\": 2}\n"
        "    VAR picked = settings[\"x\"].value\n"
        "    VAR down_reason = Actor_Down_Reason.failed\n"
        "    SELECT value\n"
        "    CASE Ok(found)\n"
        "        print(found)\n"
        "    CASE ELSE\n"
        "        print(value)\n"
        "    END SELECT\n"
        "    result = user WITH age = 2, name = $\"value {value}\"\n"
        "    RETURN result\n"
        "END FUNCTION\n"
        "ACTOR Worker\n"
        "    VAR count = 0\n"
        "    RECEIVE Tick()\n"
        "        SEND SELF, Done(count)\n"
        "        WATCH SELF\n"
        "        STOP\n"
        "    END RECEIVE\n"
        "END ACTOR\n"
        "ACTION main()\n"
        "    VAR worker = SPAWN Worker AT node\n"
        "    RETURN\n"
        "END ACTION\n";
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed,
                                 input, sizeof(input) - 1, 102));
    PARSER_ASSERT(lexed.diagnostic_count == 0);
    for (size_t index = 0; index < parsed.diagnostic_count; ++index) {
        Om_Source_Location location = {0};
        (void)om_source_text_location(&source, parsed.diagnostics[index].span.start,
                                      &location);
        (void)fprintf(stderr, "parser diagnostic %s at %u:%u\n",
                      om_parse_diagnostic_code_name(parsed.diagnostics[index].code),
                      location.line, location.byte_column);
        for (size_t token_index = 0; token_index < lexed.token_count; ++token_index) {
            if (lexed.tokens[token_index].span.start == parsed.diagnostics[index].span.start) {
                (void)fprintf(stderr, "  actual token kind=%d keyword=%d text=%.*s expected kind=%d keyword=%d\n",
                              (int)lexed.tokens[token_index].kind,
                              (int)lexed.tokens[token_index].keyword,
                              (int)lexed.tokens[token_index].lexeme.length,
                              lexed.tokens[token_index].lexeme.data,
                              (int)parsed.diagnostics[index].expected_kind,
                              (int)parsed.diagnostics[index].expected_keyword);
                break;
            }
        }
    }
    PARSER_ASSERT(parsed.diagnostic_count == 0);
    PARSER_ASSERT(ast_invariants_hold(&source, &parsed));
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_IMPORT) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_RECORD_DECLARATION) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_MESSAGE_DECLARATION) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_FUNCTION_DECLARATION) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_ACTION_DECLARATION) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_ACTOR_DECLARATION) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_RECEIVE_DECLARATION) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_IF) == 2);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_WHILE) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_FOR) == 2);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_SELECT) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_MAP) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_INDEX) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_MEMBER) == 2);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_CONTINUE) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_RETURN) >= 2);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_CALL_STATEMENT) >= 4);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_SEND) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_WATCH) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_SPAWN) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_INTERPOLATED_STRING) == 1);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_WITH) == 1);
    passed = true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_parser_reexport_import(void)
{
    static const uint8_t input[] =
        "EXPORT IMPORT \"./core.bas\"\n"
        "EXPORT FUNCTION answer()\n    RETURN 42\nEND FUNCTION\n";
    static const uint8_t invalid[] =
        "FUNCTION first()\n    RETURN 1\nEND FUNCTION\n"
        "EXPORT IMPORT \"./core.bas\"\n";
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed, input,
                                 sizeof(input) - 1, 104));
    PARSER_ASSERT(parsed.diagnostic_count == 0);
    const Om_Ast_Node *program = om_ast_get_node(&parsed.ast, parsed.ast.root);
    PARSER_ASSERT(program != NULL && program->child_count == 2);
    const Om_Ast_Node *import = om_ast_get_node(
        &parsed.ast, om_ast_get_child(&parsed.ast, program, 0));
    PARSER_ASSERT(import != NULL && import->kind == OM_AST_IMPORT
                  && (import->flags & OM_AST_FLAG_REEXPORT) != 0
                  && import->name.length == sizeof("\"./core.bas\"") - 1
                  && memcmp(import->name.data, "\"./core.bas\"",
                            sizeof("\"./core.bas\"") - 1) == 0
                  && import->span.start == 0);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed, invalid,
                                 sizeof(invalid) - 1, 105));
    PARSER_ASSERT(parsed.diagnostic_count != 0);
    passed = true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_parser_native_import(void)
{
    static const uint8_t input[] =
        "IMPORT \"./source.bas\"\n"
        "NATIVE IMPORT \"./native.omi\"\n"
        "EXPORT FUNCTION wrapper(value)\n"
        "    RETURN value\n"
        "END FUNCTION\n";
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed, input,
                                 sizeof(input) - 1, 107));
    PARSER_ASSERT(parsed.diagnostic_count == 0);
    const Om_Ast_Node *program = om_ast_get_node(&parsed.ast, parsed.ast.root);
    PARSER_ASSERT(program != NULL && program->child_count == 3);
    const Om_Ast_Node *native_import = om_ast_get_node(
        &parsed.ast, om_ast_get_child(&parsed.ast, program, 1));
    PARSER_ASSERT(native_import != NULL
                  && native_import->kind == OM_AST_NATIVE_IMPORT
                  && native_import->name.length == sizeof("\"./native.omi\"") - 1
                  && memcmp(native_import->name.data, "\"./native.omi\"",
                            sizeof("\"./native.omi\"") - 1) == 0);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);

    static const uint8_t late_import[] =
        "FUNCTION first()\n"
        "    RETURN 1\n"
        "END FUNCTION\n"
        "NATIVE IMPORT \"./native.omi\"\n";
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed, late_import,
                                 sizeof(late_import) - 1, 108));
    PARSER_ASSERT(parsed.diagnostic_count != 0);
    passed = true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_parser_errors_and_recovery(void)
{
    static const uint8_t input[] =
        "FUNCTION broken()\n"
        "    RETURN 1 < 2 < 3\n"
        "END FUNCTION\n"
        "FUNCTION good()\n"
        "    RETURN 1\n"
        "END FUNCTION\n";
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed,
                                 input, sizeof(input) - 1, 103));
    PARSER_ASSERT(lexed.diagnostic_count == 0);
    PARSER_ASSERT(parsed.diagnostic_count == 1);
    PARSER_ASSERT(parsed.diagnostics[0].code == OM_PARSE_DIAGNOSTIC_E0206);
    PARSER_ASSERT(count_kind(&parsed.ast, OM_AST_FUNCTION_DECLARATION) == 2);
    PARSER_ASSERT(ast_invariants_hold(&source, &parsed));
    PARSER_ASSERT(strcmp(om_parse_diagnostic_code_name(
                            parsed.diagnostics[0].code), "E0206") == 0);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    static const uint8_t oversized_positive[] =
        "FUNCTION invalid()\n"
        "    RETURN 9223372036854775808\n"
        "END FUNCTION\n";
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed,
                                 oversized_positive,
                                 sizeof(oversized_positive) - 1, 107));
    PARSER_ASSERT(parsed.diagnostic_count == 1);
    PARSER_ASSERT(parsed.diagnostics[0].code == OM_PARSE_DIAGNOSTIC_E0205);
    PARSER_ASSERT(ast_invariants_hold(&source, &parsed));
    passed = true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_parser_malformed_fuzz(void)
{
    static const uint8_t alphabet[] = {
        (uint8_t)'A', (uint8_t)'a', (uint8_t)'_', (uint8_t)'0', (uint8_t)'9',
        (uint8_t)' ', (uint8_t)'\t', (uint8_t)'\n', (uint8_t)'\r',
        (uint8_t)'(', (uint8_t)')', (uint8_t)'[', (uint8_t)']',
        (uint8_t)'{', (uint8_t)'}', (uint8_t)',', (uint8_t)':', (uint8_t)'.',
        (uint8_t)'=', (uint8_t)'<', (uint8_t)'>', (uint8_t)'+', (uint8_t)'-',
        (uint8_t)'*', (uint8_t)'/', (uint8_t)'%', (uint8_t)'"', (uint8_t)'$',
        UINT8_C(0x00), UINT8_C(0x80), UINT8_C(0xff)
    };
    uint32_t random = UINT32_C(0x19a2b3c4);
    for (uint32_t iteration = 0; iteration < 4096; ++iteration) {
        uint8_t input[48];
        const size_t length = (size_t)(iteration % (uint32_t)sizeof(input));
        for (size_t index = 0; index < length; ++index) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            input[index] = alphabet[random % (uint32_t)sizeof(alphabet)];
        }
        Om_Source_Text source = {0};
        Om_Lexer_Result lexed = {0};
        Om_Parser_Result parsed = {0};
        if (!scan_and_parse(&source, &lexed, &parsed, input, length,
                            iteration + UINT32_C(1000))) {
            om_parser_result_destroy(&parsed);
            om_lexer_result_destroy(&lexed);
            om_source_text_destroy(&source);
            return false;
        }
        const bool valid = ast_invariants_hold(&source, &parsed);
        om_parser_result_destroy(&parsed);
        om_lexer_result_destroy(&lexed);
        om_source_text_destroy(&source);
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool om_test_parser_depth_limit(void)
{
    enum { NESTING = 1400, BUFFER_SIZE = NESTING * 2 + 64 };
    uint8_t input[BUFFER_SIZE];
    size_t cursor = 0;
    static const char prefix[] = "FUNCTION deep()\nRETURN ";
    static const char ending[] = "\nEND FUNCTION\n";
    (void)memcpy(input + cursor, prefix, sizeof(prefix) - 1);
    cursor += sizeof(prefix) - 1;
    for (size_t index = 0; index < NESTING; ++index) {
        input[cursor++] = (uint8_t)'(';
    }
    input[cursor++] = (uint8_t)'1';
    for (size_t index = 0; index < NESTING; ++index) {
        input[cursor++] = (uint8_t)')';
    }
    (void)memcpy(input + cursor, ending, sizeof(ending) - 1);
    cursor += sizeof(ending) - 1;

    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed, input, cursor, 104));
    PARSER_ASSERT(parsed.diagnostic_count > 0);
    PARSER_ASSERT(ast_invariants_hold(&source, &parsed));
    passed = true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_parser_block_depth_limit(void)
{
    enum { NESTING = 700, BUFFER_SIZE = NESTING * 40 + 128 };
    static const char prefix[] = "FUNCTION deep()\n";
    static const char if_line[] = "IF TRUE THEN\n";
    static const char return_line[] = "RETURN 0\n";
    static const char end_line[] = "END IF\n";
    static const char ending[] = "END FUNCTION\n";
    uint8_t input[BUFFER_SIZE];
    size_t cursor = 0;
    (void)memcpy(input + cursor, prefix, sizeof(prefix) - 1);
    cursor += sizeof(prefix) - 1;
    for (size_t index = 0; index < NESTING; ++index) {
        (void)memcpy(input + cursor, if_line, sizeof(if_line) - 1);
        cursor += sizeof(if_line) - 1;
    }
    (void)memcpy(input + cursor, return_line, sizeof(return_line) - 1);
    cursor += sizeof(return_line) - 1;
    for (size_t index = 0; index < NESTING; ++index) {
        (void)memcpy(input + cursor, end_line, sizeof(end_line) - 1);
        cursor += sizeof(end_line) - 1;
    }
    (void)memcpy(input + cursor, ending, sizeof(ending) - 1);
    cursor += sizeof(ending) - 1;

    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed, input, cursor, 105));
    PARSER_ASSERT(parsed.diagnostic_count > 0);
    PARSER_ASSERT(ast_invariants_hold(&source, &parsed));
    passed = true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_parser_conformance_fixture(void)
{
    char source_path[1024] = {0};
    uint8_t *source_bytes = NULL;
    size_t source_length = 0;
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(snprintf(source_path, sizeof(source_path),
                           "%s/chained_comparison.bas",
                           OM_PARSER_GOLDEN_ROOT) > 0);
    PARSER_ASSERT(read_file(source_path, &source_bytes, &source_length));
    PARSER_ASSERT(om_source_text_init(
        &source, (Om_Source_Id){106}, (Om_String_View){"chained_comparison.bas", 22},
        (Om_Const_Byte_Slice){source_bytes, source_length}, 0
    ) == OM_STATUS_OK);
    PARSER_ASSERT(om_lexer_scan(&source, &lexed) == OM_STATUS_OK);
    PARSER_ASSERT(lexed.diagnostic_count == 0);
    PARSER_ASSERT(om_parser_parse(&lexed, &parsed) == OM_STATUS_OK);
    PARSER_ASSERT(parsed.diagnostic_count == 1);
    PARSER_ASSERT(parsed.diagnostics[0].code == OM_PARSE_DIAGNOSTIC_E0206);
    Om_Const_Byte_Slice selected = {0};
    PARSER_ASSERT(om_source_text_slice(&source, parsed.diagnostics[0].span,
                                       &selected) == OM_STATUS_OK);
    PARSER_ASSERT(selected.length == 1 && selected.data[0] == (uint8_t)'<');
    PARSER_ASSERT(ast_invariants_hold(&source, &parsed));
    passed = true;

cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    om_free(source_bytes);
    return passed;
}

bool om_test_parser_query_expression(void)
{
    static const uint8_t input[] =
        "ACTION main()\n"
        "    VAR result = QUERY item IN items\n"
        "        WHERE item > 0\n"
        "        WHERE item < 100\n"
        "        ORDER BY item DESCENDING, item ASCENDING\n"
        "        TAKE 10\n"
        "        SELECT item\n"
        "    END QUERY\n"
        "END ACTION\n";
    Om_Source_Text source = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    bool passed = false;
    PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed, input,
                                 sizeof(input) - 1, 41));
    PARSER_ASSERT(lexed.diagnostic_count == 0);
    PARSER_ASSERT(parsed.diagnostic_count == 0);
    const Om_Ast_Node *query = NULL;
    for (size_t index = 0; index < parsed.ast.node_count; ++index) {
        if (parsed.ast.nodes[index].kind == OM_AST_QUERY) {
            query = &parsed.ast.nodes[index];
            break;
        }
    }
    PARSER_ASSERT(query != NULL && query->child_count == 6);
    PARSER_ASSERT(ast_child_node(&parsed.ast, query, 0)->kind == OM_AST_NAME);
    PARSER_ASSERT(ast_child_node(&parsed.ast, query, 1)->kind == OM_AST_QUERY_WHERE);
    PARSER_ASSERT(ast_child_node(&parsed.ast, query, 2)->kind == OM_AST_QUERY_WHERE);
    const Om_Ast_Node *order = ast_child_node(&parsed.ast, query, 3);
    PARSER_ASSERT(order != NULL && order->kind == OM_AST_QUERY_ORDER
                  && order->child_count == 2);
    const Om_Ast_Node *descending = ast_child_node(&parsed.ast, order, 0);
    const Om_Ast_Node *ascending = ast_child_node(&parsed.ast, order, 1);
    PARSER_ASSERT(descending != NULL
                  && (descending->flags & OM_AST_FLAG_QUERY_DESCENDING) != 0);
    PARSER_ASSERT(ascending != NULL
                  && (ascending->flags & OM_AST_FLAG_QUERY_DESCENDING) == 0);
    PARSER_ASSERT(ast_child_node(&parsed.ast, query, 4)->kind == OM_AST_QUERY_TAKE);
    PARSER_ASSERT(ast_child_node(&parsed.ast, query, 5)->kind == OM_AST_QUERY_SELECT);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    static const char *const invalid_queries[] = {
        "ACTION main()\n"
        "    result = QUERY item IN items\n"
        "        WHERE item > 0\n"
        "    END QUERY\n"
        "END ACTION\n",
        "ACTION main()\n"
        "    result = QUERY item IN items\n"
        "        ORDER BY item\n"
        "        SELECT item\n"
        "    END QUERY\n"
        "END ACTION\n",
        "ACTION main()\n"
        "    result = QUERY item IN items\n"
        "        SELECT item\n"
        "        TAKE 1\n"
        "    END QUERY\n"
        "END ACTION\n",
        "ACTION main()\n"
        "    result = QUERY item IN items\n"
        "        ORDER BY item ASCENDING\n"
        "        ORDER BY item DESCENDING\n"
        "        SELECT item\n"
        "    END QUERY\n"
        "END ACTION\n",
        "ACTION main()\n"
        "    result = QUERY item IN items\n"
        "        TAKE 1\n"
        "        TAKE 2\n"
        "        SELECT item\n"
        "    END QUERY\n"
        "END ACTION\n",
        "ACTION main()\n"
        "    result = QUERY item IN items\n"
        "        TAKE 1\n"
        "        ORDER BY item ASCENDING\n"
        "        SELECT item\n"
        "    END QUERY\n"
        "END ACTION\n",
    };
    static const Om_Parse_Diagnostic_Code expected_codes[] = {
        OM_PARSE_DIAGNOSTIC_E0209,
        OM_PARSE_DIAGNOSTIC_E0213,
        OM_PARSE_DIAGNOSTIC_E0210,
        OM_PARSE_DIAGNOSTIC_E0211,
        OM_PARSE_DIAGNOSTIC_E0212,
        OM_PARSE_DIAGNOSTIC_E0210,
    };
    for (size_t index = 0;
         index < sizeof(invalid_queries) / sizeof(invalid_queries[0]);
         ++index) {
        const size_t length = strlen(invalid_queries[index]);
        PARSER_ASSERT(scan_and_parse(&source, &lexed, &parsed,
            (const uint8_t *)invalid_queries[index], length,
            (uint32_t)(42 + index)));
        PARSER_ASSERT(parsed.diagnostic_count == 1
            && parsed.diagnostics[0].code == expected_codes[index]);
        om_parser_result_destroy(&parsed);
        om_lexer_result_destroy(&lexed);
        om_source_text_destroy(&source);
    }
    passed = true;
cleanup:
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_source_text_destroy(&source);
    return passed;
}
