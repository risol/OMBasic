#include "cli_internal.h"

#include <limits.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/bytecode/bytecode.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/runtime/vm.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/tooling/diagnostics_json.h"
#include "ombasic/tooling/interface_cache.h"
#include "ombasic/tooling/type_format.h"

typedef struct Om_Cli_Diagnostic_List {
    Om_Tooling_Diagnostic *items;
    size_t length;
    size_t capacity;
} Om_Cli_Diagnostic_List;

static Om_Status read_stdin_source(uint8_t **bytes, size_t *length)
{
    size_t capacity = 0;
    size_t used = 0;
    uint8_t *data = NULL;
    if (bytes == NULL || length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *bytes = NULL;
    *length = 0;
    for (;;) {
        if (capacity - used < 4096) {
            size_t next_capacity = capacity == 0 ? 4096 : capacity * 2;
            if (next_capacity > OM_SOURCE_TEXT_DEFAULT_MAX_BYTES) {
                next_capacity = OM_SOURCE_TEXT_DEFAULT_MAX_BYTES;
            }
            if (next_capacity <= capacity) {
                om_free(data);
                return OM_STATUS_OUT_OF_RANGE;
            }
            uint8_t *next = om_realloc_array(data, next_capacity, sizeof(*next));
            if (next == NULL) {
                om_free(data);
                return OM_STATUS_OUT_OF_MEMORY;
            }
            data = next;
            capacity = next_capacity;
        }
        const size_t count = fread(data + used, 1, capacity - used, stdin);
        used += count;
        if (count == 0) {
            if (ferror(stdin) != 0) {
                om_free(data);
                return OM_STATUS_IO_ERROR;
            }
            break;
        }
        if (used == OM_SOURCE_TEXT_DEFAULT_MAX_BYTES) {
            const int next = fgetc(stdin);
            if (next != EOF) {
                om_free(data);
                return OM_STATUS_OUT_OF_RANGE;
            }
            if (ferror(stdin) != 0) {
                om_free(data);
                return OM_STATUS_IO_ERROR;
            }
            break;
        }
    }
    if (data == NULL) {
        data = om_alloc(1);
        if (data == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    *bytes = data;
    *length = used;
    return OM_STATUS_OK;
}

static Om_String_View literal_view(const char *value)
{
    return (Om_String_View){value, strlen(value)};
}

static const Om_Source_Text *source_for_module(const Om_Module_Project *project,
                                               Om_Module_Id module_id)
{
    return om_module_project_source(project, module_id);
}

static Om_Status push_diagnostic(Om_Cli_Diagnostic_List *list,
                                 Om_Tooling_Diagnostic diagnostic)
{
    if (list == NULL || diagnostic.source == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (list->length == list->capacity) {
        const size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        if (capacity < list->capacity) {
            return OM_STATUS_OVERFLOW;
        }
        Om_Tooling_Diagnostic *items = om_realloc_array(
            list->items, capacity, sizeof(*items));
        if (items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->length++] = diagnostic;
    return OM_STATUS_OK;
}

static const char *lex_message(Om_Lex_Diagnostic_Code code)
{
    switch (code) {
    case OM_LEX_DIAGNOSTIC_E0101:
        return "Reserved keyword spelling must use exact uppercase casing.";
    case OM_LEX_DIAGNOSTIC_E0102:
        return "Identifier does not match an ordinary or entity name pattern.";
    case OM_LEX_DIAGNOSTIC_E0103:
        return "Malformed or unsupported lexical token.";
    case OM_LEX_DIAGNOSTIC_E0104:
        return "Numeric literal is outside the supported Int64 or finite Float range.";
    default:
        return "Lexical error.";
    }
}

static const char *parse_message(Om_Parse_Diagnostic_Code code)
{
    switch (code) {
    case OM_PARSE_DIAGNOSTIC_E0202:
        return "Expected a different token or found malformed syntax.";
    case OM_PARSE_DIAGNOSTIC_E0203:
        return "Unsupported top-level construct or import placement.";
    case OM_PARSE_DIAGNOSTIC_E0204:
        return "An expression is required at this position.";
    case OM_PARSE_DIAGNOSTIC_E0205:
        return "The 2^63 integer magnitude must be the direct operand of unary minus.";
    case OM_PARSE_DIAGNOSTIC_E0206:
        return "A comparison expression cannot contain a comparison chain.";
    case OM_PARSE_DIAGNOSTIC_E0207:
        return "A standalone expression statement must be a call.";
    case OM_PARSE_DIAGNOSTIC_E0208:
        return "Parser recovery or nesting safety limit was reached.";
    default:
        return "Syntax error.";
    }
}

static const char *resolution_message(Om_Resolution_Diagnostic_Code code)
{
    switch (code) {
    case OM_RESOLUTION_DIAGNOSTIC_E0301:
        return "Name or qualified module member could not be resolved.";
    case OM_RESOLUTION_DIAGNOSTIC_E0302:
        return "Imported module member exists but is private.";
    case OM_RESOLUTION_DIAGNOSTIC_E0303:
        return "Name is declared more than once in the same scope.";
    case OM_RESOLUTION_DIAGNOSTIC_E0304:
        return "Declaration conflicts with a visible name using different casing.";
    case OM_RESOLUTION_DIAGNOSTIC_E0305:
        return "Declaration shadows a name visible from an enclosing scope or prelude.";
    case OM_RESOLUTION_DIAGNOSTIC_E0306:
        return "Module alias is invalid or conflicts with another declaration.";
    case OM_RESOLUTION_DIAGNOSTIC_E0307:
        return "Assignment targets an immutable binding.";
    default:
        return "Name resolution error.";
    }
}

static const char *type_message(Om_Type_Inference_Diagnostic_Code code)
{
    switch (code) {
    case OM_TYPE_INFERENCE_DIAGNOSTIC_E0401:
        return "Expression, argument, field, return value, or exported field has an incompatible or unstable type.";
    case OM_TYPE_INFERENCE_DIAGNOSTIC_E0402:
        return "Empty collection has no contextual type for inference.";
    default:
        return "Type inference error.";
    }
}

static const char *semantic_message(Om_Semantic_Analysis_Diagnostic_Code code)
{
    switch (code) {
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501:
        return "Effect is not allowed in this context.";
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601:
        return "Actor does not accept this Message.";
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701:
        return "Message payload contains a non-serializable value.";
    default:
        return "Semantic validation error.";
    }
}

static Om_Status collect_lex_diagnostics(Om_Cli_Diagnostic_List *list,
                                         const Om_Source_Text *source,
                                         const Om_Lexer_Result *lexed)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < lexed->diagnostic_count; ++index) {
        const Om_Lex_Diagnostic *item = &lexed->diagnostics[index];
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"lexer", 5},
            .code = literal_view(om_lex_diagnostic_code_name(item->code)),
            .message = literal_view(lex_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static Om_Status collect_parse_diagnostics(Om_Cli_Diagnostic_List *list,
                                           const Om_Source_Text *source,
                                           const Om_Parser_Result *parsed)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < parsed->diagnostic_count; ++index) {
        const Om_Parse_Diagnostic *item = &parsed->diagnostics[index];
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"parser", 6},
            .code = literal_view(om_parse_diagnostic_code_name(item->code)),
            .message = literal_view(parse_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static Om_Status collect_resolution_diagnostics(Om_Cli_Diagnostic_List *list,
                                                const Om_Module_Project *project,
                                                const Om_Resolution_Result *result)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < result->diagnostic_count; ++index) {
        const Om_Resolution_Diagnostic *item = &result->diagnostics[index];
        const Om_Source_Text *source = source_for_module(project, item->module_id);
        const Om_Source_Text *related = item->has_related_span
            ? source_for_module(project, item->related_span.source.value == UINT32_MAX
                ? item->module_id : (Om_Module_Id){item->related_span.source.value})
            : NULL;
        if (source == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"resolution", 10},
            .code = literal_view(om_resolution_diagnostic_code_name(item->code)),
            .message = literal_view(resolution_message(item->code)),
            .source = source,
            .span = item->span,
            .has_related_span = item->has_related_span,
            .related_source = related,
            .related_span = item->related_span
        });
    }
    return status;
}

static Om_Status collect_type_diagnostics(Om_Cli_Diagnostic_List *list,
                                          const Om_Module_Project *project,
                                          const Om_Type_Inference_Result *result)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < result->diagnostic_count; ++index) {
        const Om_Type_Inference_Diagnostic *item = &result->diagnostics[index];
        const Om_Source_Text *source = source_for_module(project, item->module_id);
        if (source == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"type", 4},
            .code = literal_view(om_type_inference_diagnostic_code_name(item->code)),
            .message = literal_view(type_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static Om_Status collect_semantic_diagnostics(Om_Cli_Diagnostic_List *list,
                                              const Om_Module_Project *project,
                                              const Om_Semantic_Analysis_Result *result)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < result->diagnostic_count; ++index) {
        const Om_Semantic_Analysis_Diagnostic *item = &result->diagnostics[index];
        const Om_Source_Text *source = source_for_module(project, item->module_id);
        if (source == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"semantic", 8},
            .code = literal_view(om_semantic_analysis_diagnostic_code_name(item->code)),
            .message = literal_view(semantic_message(item->code)),
            .source = source,
            .span = item->span
        });
    }
    return status;
}

static void print_diagnostics(const Om_Cli_Diagnostic_List *list, bool json)
{
    if (json) {
        Om_Tooling_Json_Result result = {0};
        const Om_Status status = om_tooling_diagnostics_to_json(
            list->items, list->length, &result);
        if (status == OM_STATUS_OK) {
            (void)fwrite(result.data, 1, result.length, stdout);
            (void)fputc('\n', stdout);
        } else {
            (void)fprintf(stderr, "ombasic: could not encode diagnostics (%s)\n",
                          om_status_name(status));
        }
        om_tooling_json_result_destroy(&result);
        return;
    }
    for (size_t index = 0; index < list->length; ++index) {
        const Om_Tooling_Diagnostic *diagnostic = &list->items[index];
        Om_Source_Location location = {0};
        (void)om_source_text_location(diagnostic->source, diagnostic->span.start,
                                      &location);
        const Om_String_View path = diagnostic->source->canonical_path;
        (void)fprintf(stderr, "%.*s:%u:%u: %.*s: %.*s\n",
                      path.length > INT_MAX ? INT_MAX : (int)path.length,
                      path.data, location.line, location.byte_column,
                      (int)diagnostic->code.length, diagnostic->code.data,
                      (int)diagnostic->message.length, diagnostic->message.data);
    }
}

static Om_Status print_json_string(Om_String_View value)
{
    static const char hex[] = "0123456789abcdef";
    if (fputc('"', stdout) == EOF) {
        return OM_STATUS_IO_ERROR;
    }
    for (size_t index = 0; index < value.length; ++index) {
        const unsigned char byte = (unsigned char)value.data[index];
        if (byte == '"' || byte == '\\') {
            if (fputc('\\', stdout) == EOF || fputc(byte, stdout) == EOF) {
                return OM_STATUS_IO_ERROR;
            }
        } else if (byte == '\n' || byte == '\r' || byte == '\t') {
            const char escaped = byte == '\n' ? 'n' : byte == '\r' ? 'r' : 't';
            if (fputc('\\', stdout) == EOF || fputc(escaped, stdout) == EOF) {
                return OM_STATUS_IO_ERROR;
            }
        } else if (byte < UINT8_C(0x20)) {
            const char escaped[6] = {
                '\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0x0f]
            };
            if (fwrite(escaped, 1, sizeof(escaped), stdout) != sizeof(escaped)) {
                return OM_STATUS_IO_ERROR;
            }
        } else if (fputc(byte, stdout) == EOF) {
            return OM_STATUS_IO_ERROR;
        }
    }
    return fputc('"', stdout) == EOF ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
}

static const char *ast_kind_name(Om_Ast_Kind kind)
{
    switch (kind) {
    case OM_AST_BINDING: return "binding";
    case OM_AST_NAME: return "name";
    case OM_AST_PARAMETER: return "parameter";
    case OM_AST_CALL: return "call";
    case OM_AST_MEMBER: return "member";
    case OM_AST_INDEX: return "index";
    case OM_AST_BINARY: return "binary";
    case OM_AST_UNARY: return "unary";
    case OM_AST_INTEGER_LITERAL: return "integer";
    case OM_AST_FLOAT_LITERAL: return "float";
    case OM_AST_STRING_LITERAL: return "string";
    case OM_AST_BOOLEAN_LITERAL: return "boolean";
    case OM_AST_LIST: return "list";
    case OM_AST_MAP: return "map";
    case OM_AST_SPAWN: return "spawn";
    default: return "expression";
    }
}

static const char *symbol_kind_name(Om_Symbol_Kind kind)
{
    switch (kind) {
    case OM_SYMBOL_FUNCTION: return "function";
    case OM_SYMBOL_ACTION: return "action";
    case OM_SYMBOL_RECORD: return "record";
    case OM_SYMBOL_MESSAGE: return "message";
    case OM_SYMBOL_ACTOR: return "actor";
    default: return "symbol";
    }
}

static Om_Status write_inferred_nodes(const Om_Module_Project *project,
                                      const Om_Parser_Result *parsed,
                                      size_t module_count,
                                      const Om_Resolution_Result *resolution,
                                      const Om_Type_Inference_Result *inference)
{
    Om_Status status = OM_STATUS_OK;
    bool first = true;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < inference->inferred_node_count; ++index) {
        const Om_Inferred_Node *inferred = &inference->inferred_nodes[index];
        if (inferred->module_id.value >= module_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Source_Text *source = source_for_module(project, inferred->module_id);
        const Om_Ast *ast = &parsed[inferred->module_id.value].ast;
        const Om_Ast_Node *node = om_ast_get_node(ast, inferred->node_id);
        if (source == NULL || node == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Tooling_String type = {0};
        status = om_tooling_type_to_string(&inference->type_store, resolution,
                                           inferred->type_id, &type);
        Om_Source_Location start = {0};
        Om_Source_Location end = {0};
        if (status == OM_STATUS_OK) {
            status = om_source_text_location(source, node->span.start, &start);
        }
        if (status == OM_STATUS_OK) {
            status = om_source_text_location(source, node->span.end, &end);
        }
        if (status == OM_STATUS_OK && !first) {
            status = fputc(',', stdout) == EOF ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = fputs("{\"path\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = print_json_string(source->canonical_path);
        }
        if (status == OM_STATUS_OK) {
            status = fputs(",\"kind\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            const char *kind = ast_kind_name(node->kind);
            status = print_json_string((Om_String_View){kind, strlen(kind)});
        }
        if (status == OM_STATUS_OK) {
            status = fputs(",\"name\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = print_json_string(node->name);
        }
        if (status == OM_STATUS_OK) {
            status = fputs(",\"type\":", stdout) == EOF
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = print_json_string((Om_String_View){type.data, type.length});
        }
        if (status == OM_STATUS_OK) {
            status = fprintf(stdout,
                ",\"range\":{\"start\":{\"line\":%u,\"byteColumn\":%u},"
                "\"end\":{\"line\":%u,\"byteColumn\":%u}}}",
                start.line, start.byte_column, end.line, end.byte_column) < 0
                ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            first = false;
        }
        om_tooling_string_destroy(&type);
    }
    return status;
}

static void print_analysis_json(const Om_Cli_Diagnostic_List *list,
                                const Om_Module_Project *project,
                                const Om_Parser_Result *parsed,
                                size_t module_count,
                                const Om_Resolution_Result *resolution,
                                const Om_Type_Inference_Result *inference,
                                const Om_Interface_Cache *interfaces,
                                bool include_inferred,
                                bool include_interfaces)
{
    Om_Tooling_Json_Result diagnostics_json = {0};
    const Om_Status status = om_tooling_diagnostics_to_json(
        list->items, list->length, &diagnostics_json);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: could not encode diagnostics (%s)\n",
                      om_status_name(status));
        goto cleanup;
    }
    if (!include_inferred && !include_interfaces) {
        (void)fwrite(diagnostics_json.data, 1, diagnostics_json.length, stdout);
        (void)fputc('\n', stdout);
        goto cleanup;
    }
    static const char diagnostics_open[] = "{\"diagnostics\":[";
    if (diagnostics_json.length < sizeof(diagnostics_open)
        || memcmp(diagnostics_json.data, diagnostics_open,
                  sizeof(diagnostics_open) - 1) != 0) {
        (void)fprintf(stderr, "ombasic: invalid diagnostics JSON\n");
        goto cleanup;
    }
    (void)fwrite(diagnostics_json.data, 1, diagnostics_json.length - 1, stdout);
    (void)fputs(",\"inferred\":[", stdout);
    if (include_inferred && inference->type_store.owned_storage != NULL
        && resolution->symbols != NULL) {
        const Om_Status inferred_status = write_inferred_nodes(
            project, parsed, module_count, resolution, inference);
        if (inferred_status != OM_STATUS_OK) {
            (void)fprintf(stderr, "ombasic: could not format inferred types (%s)\n",
                          om_status_name(inferred_status));
            goto cleanup;
        }
    }
    (void)fputs("],\"interfaces\":[", stdout);
    if (include_interfaces && interfaces->modules != NULL) {
        for (size_t module_index = 0; module_index < interfaces->module_count;
             ++module_index) {
            const Om_Module_Interface *module = &interfaces->modules[module_index];
            if (module_index != 0) {
                (void)fputc(',', stdout);
            }
            (void)fputs("{\"path\":", stdout);
            (void)print_json_string(module->path);
            (void)fprintf(stdout, ",\"sourceHash\":\"%" PRIu64
                          "\",\"interfaceHash\":\"%" PRIu64 "\",\"symbols\":[",
                          module->source_hash, module->interface_hash);
            for (size_t symbol_index = 0; symbol_index < module->symbol_count;
                 ++symbol_index) {
                const Om_Interface_Symbol *symbol = &module->symbols[symbol_index];
                if (symbol_index != 0) {
                    (void)fputc(',', stdout);
                }
                (void)fputs("{\"name\":", stdout);
                (void)print_json_string(symbol->name);
                (void)fputs(",\"kind\":", stdout);
                const char *kind = symbol_kind_name(symbol->kind);
                (void)print_json_string((Om_String_View){kind, strlen(kind)});
                (void)fprintf(stdout, ",\"effects\":%" PRIu32,
                              symbol->effects);
                if (symbol->type_scheme != NULL
                    && inference->type_store.owned_storage != NULL) {
                    Om_Tooling_String type = {0};
                    const Om_Status type_status = om_tooling_type_to_string(
                        &inference->type_store, resolution,
                        symbol->type_scheme->type_id, &type);
                    if (type_status == OM_STATUS_OK) {
                        (void)fputs(",\"type\":", stdout);
                        (void)print_json_string((Om_String_View){type.data, type.length});
                    } else {
                        (void)fputs(",\"type\":null", stdout);
                    }
                    om_tooling_string_destroy(&type);
                } else {
                    (void)fputs(",\"type\":null", stdout);
                }
                (void)fprintf(stdout, ",\"hasSerializableType\":%s,\"serializable\":%s}",
                    symbol->has_serializability ? "true" : "false",
                    symbol->serializable ? "true" : "false");
            }
            (void)fputs("],\"actorProtocol\":[", stdout);
            for (size_t protocol_index = 0;
                 protocol_index < module->actor_protocol_count; ++protocol_index) {
                const Om_Actor_Protocol_Entry *entry = &module->actor_protocol[protocol_index];
                const Om_Symbol *actor = om_resolution_symbol_by_id(
                    resolution, entry->actor_symbol);
                const Om_Symbol *message = om_resolution_symbol_by_id(
                    resolution, entry->message_symbol);
                if (actor == NULL || message == NULL) {
                    continue;
                }
                if (protocol_index != 0) {
                    (void)fputc(',', stdout);
                }
                (void)fputs("{\"actor\":", stdout);
                (void)print_json_string(actor->name);
                (void)fputs(",\"message\":", stdout);
                (void)print_json_string(message->name);
                (void)fputc('}', stdout);
            }
            (void)fputs("]}", stdout);
        }
    }
    (void)fputs("]}\n", stdout);
cleanup:
    om_tooling_json_result_destroy(&diagnostics_json);
}

static bool write_encoded_bytecode(const char *path, Om_Const_Byte_Slice bytes)
{
    FILE *file = NULL;
#ifdef _MSC_VER
    if (fopen_s(&file, path, "wb") != 0) {
        return false;
    }
#else
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
#endif
    const bool wrote_all = fwrite(bytes.data, 1, bytes.length, file) == bytes.length;
    const bool closed = fclose(file) == 0;
    return wrote_all && closed;
}

static const char *backend_diagnostic_code(Om_Bytecode_Emit_Code code)
{
    switch (code) {
    case OM_BYTECODE_EMIT_UNSUPPORTED_NODE: return "E0801";
    case OM_BYTECODE_EMIT_UNRESOLVED_SYMBOL: return "E0802";
    case OM_BYTECODE_EMIT_INVALID_INPUT: return "E0803";
    default: return "E0899";
    }
}

static const char *backend_diagnostic_message(Om_Bytecode_Emit_Code code)
{
    switch (code) {
    case OM_BYTECODE_EMIT_UNSUPPORTED_NODE:
        return "This valid source construct is not supported by the bytecode emitter.";
    case OM_BYTECODE_EMIT_UNRESOLVED_SYMBOL:
        return "The bytecode emitter could not resolve a semantic symbol.";
    case OM_BYTECODE_EMIT_INVALID_INPUT:
        return "The bytecode emitter received an invalid compiler model.";
    default:
        return "Bytecode emission failed.";
    }
}

static Om_Status collect_backend_diagnostics(Om_Cli_Diagnostic_List *list,
                                             const Om_Module_Project *project,
                                             const Om_Parser_Result *parsed,
                                             size_t module_count,
                                             const Om_Bytecode_Emit_Result *emitted)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < emitted->diagnostic_count; ++index) {
        const Om_Bytecode_Emit_Diagnostic *item = &emitted->diagnostics[index];
        if (item->module_id.value >= module_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Source_Text *source = source_for_module(project, item->module_id);
        const Om_Ast_Node *node = om_ast_get_node(
            &parsed[item->module_id.value].ast, item->node_id);
        if (source == NULL || node == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = push_diagnostic(list, (Om_Tooling_Diagnostic){
            .stage = {"bytecode", 8},
            .code = literal_view(backend_diagnostic_code(item->code)),
            .message = literal_view(backend_diagnostic_message(item->code)),
            .source = source,
            .span = node->span
        });
    }
    return status;
}

static const Om_Source_Text *source_for_source_id(const Om_Module_Project *project,
                                                 Om_Source_Id source_id)
{
    const size_t count = om_module_project_module_count(project);
    for (size_t index = 0; index < count; ++index) {
        const Om_Source_Text *source = source_for_module(
            project, (Om_Module_Id){(uint32_t)index});
        if (source != NULL && source->id.value == source_id.value) {
            return source;
        }
    }
    return NULL;
}

static bool print_execution_value(Om_VM_Value value)
{
    switch (value.tag) {
    case OM_VM_VALUE_UNIT:
        return true;
    case OM_VM_VALUE_NULL:
        return fputs("NULL\n", stdout) != EOF;
    case OM_VM_VALUE_BOOL:
        return fprintf(stdout, "%s\n", value.as.boolean ? "TRUE" : "FALSE") >= 0;
    case OM_VM_VALUE_INT:
        return fprintf(stdout, "%" PRId64 "\n", value.as.integer) >= 0;
    case OM_VM_VALUE_FLOAT:
        return fprintf(stdout, "%.17g\n", value.as.floating) >= 0;
    case OM_VM_VALUE_OBJECT: {
        Om_String_View string = {0};
        if (om_vm_value_as_string(value, &string)) {
            return fwrite(string.data, 1, string.length, stdout) == string.length
                && fputc('\n', stdout) != EOF;
        }
        return false;
    }
    default:
        return false;
    }
}

static int run_bytecode(const Om_Module_Project *project,
                        const Om_Bytecode_Image *image,
                        Om_String_View entry_name)
{
    Om_VM vm = {0};
    uint32_t function_index = 0;
    Om_Status status = om_vm_find_function(image, entry_name, &function_index);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: entry function '%.*s' was not found\n",
                      entry_name.length > INT_MAX ? INT_MAX : (int)entry_name.length,
                      entry_name.data);
        return 1;
    }
    status = om_vm_init(&vm, image, (Om_VM_Limits){0});
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: could not initialize VM (%s)\n",
                      om_status_name(status));
        return 1;
    }
    Om_VM_Execution_Result execution = {0};
    status = om_vm_execute(&vm, function_index, NULL, 0, &execution);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: VM execution failed (%s)\n",
                      om_status_name(status));
        om_vm_destroy(&vm);
        return 1;
    }
    if (!execution.succeeded) {
        const Om_Source_Text *source = execution.error.has_source_span
            ? source_for_source_id(project, execution.error.source_span.source) : NULL;
        Om_Source_Location location = {0};
        if (source != NULL) {
            (void)om_source_text_location(source, execution.error.source_span.start,
                                          &location);
            (void)fprintf(stderr, "%.*s:%u:%u: runtime error: %s\n",
                          source->canonical_path.length > INT_MAX
                              ? INT_MAX : (int)source->canonical_path.length,
                          source->canonical_path.data, location.line,
                          location.byte_column,
                          om_vm_runtime_error_name(execution.error.code));
        } else {
            (void)fprintf(stderr, "ombasic: runtime error: %s\n",
                          om_vm_runtime_error_name(execution.error.code));
        }
        om_vm_destroy(&vm);
        return 1;
    }
    const bool printed = print_execution_value(execution.value);
    om_vm_destroy(&vm);
    if (!printed) {
        (void)fprintf(stderr,
                      "ombasic: entry returned a value that the CLI cannot display\n");
        return 1;
    }
    return 0;
}

int om_cli_compile_command(int argc, char **argv, const char *command)
{
    Om_Source_Root root = {0};
    Om_Module_Project project = {0};
    Om_Lexer_Result *lexed = NULL;
    Om_Parser_Result *parsed = NULL;
    Om_Resolution_Result resolution = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result semantic = {0};
    Om_Interface_Cache interfaces = {0};
    Om_Bytecode_Emit_Result emitted = {0};
    Om_Byte_Slice encoded = {0};
    Om_Cli_Diagnostic_List diagnostics = {0};
    uint8_t *entry_source = NULL;
    size_t entry_source_length = 0;
    Om_Status status = OM_STATUS_OK;
    const char *root_path = NULL;
    const char *entry_path = NULL;
    const char *output_path = "out.ombc";
    const char *entry_name = "main";
    bool json = false;
    bool read_source_stdin = false;
    bool include_inferred = false;
    bool include_interfaces = false;
    bool all_syntax_valid = true;
    bool semantic_valid = false;
    int exit_code = 1;

    for (int index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--root") == 0 && index + 1 < argc) {
            root_path = argv[++index];
        } else if (strcmp(argv[index], "--diagnostics=json") == 0) {
            json = true;
        } else if (strcmp(argv[index], "--source-stdin") == 0) {
            read_source_stdin = true;
        } else if (strcmp(argv[index], "--include-inferred") == 0) {
            include_inferred = true;
        } else if (strcmp(argv[index], "--include-interfaces") == 0) {
            include_interfaces = true;
        } else if (strcmp(command, "build") == 0
                   && (strcmp(argv[index], "--output") == 0
                       || strcmp(argv[index], "-o") == 0)
                   && index + 1 < argc) {
            output_path = argv[++index];
        } else if (strcmp(command, "run") == 0
                   && strcmp(argv[index], "--entry") == 0 && index + 1 < argc) {
            entry_name = argv[++index];
        } else if (argv[index][0] != '-' && entry_path == NULL) {
            entry_path = argv[index];
        } else {
            (void)fprintf(stderr,
                "Usage: ombasic %s --root <source-root> <entry.bas>\n", command);
            goto cleanup;
        }
    }
    if (root_path == NULL || entry_path == NULL) {
        (void)fprintf(stderr,
            "Usage: ombasic %s --root <source-root> <entry.bas>\n", command);
        goto cleanup;
    }
    status = om_source_root_init(&root, literal_view(root_path));
    if (status == OM_STATUS_OK && read_source_stdin) {
        status = read_stdin_source(&entry_source, &entry_source_length);
    }
    if (status == OM_STATUS_OK && read_source_stdin) {
        status = om_module_project_load_with_entry_source(
            &root, literal_view(entry_path),
            (Om_Const_Byte_Slice){entry_source, entry_source_length},
            OM_SOURCE_TEXT_DEFAULT_MAX_BYTES, &project);
    } else if (status == OM_STATUS_OK) {
        status = om_module_project_load(&root, literal_view(entry_path),
                                        OM_SOURCE_TEXT_DEFAULT_MAX_BYTES, &project);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: cannot load project (%s)\n",
                      om_status_name(status));
        goto cleanup;
    }

    const size_t module_count = om_module_project_module_count(&project);
    if (module_count == 0 || module_count > SIZE_MAX / sizeof(*lexed)) {
        status = OM_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    lexed = om_alloc_array(module_count, sizeof(*lexed));
    parsed = om_alloc_array(module_count, sizeof(*parsed));
    if (lexed == NULL || parsed == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    (void)memset(lexed, 0, module_count * sizeof(*lexed));
    (void)memset(parsed, 0, module_count * sizeof(*parsed));
    for (size_t index = 0; index < module_count; ++index) {
        const Om_Module_Id module_id = {(uint32_t)index};
        const Om_Source_Text *source = source_for_module(&project, module_id);
        if (source == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        status = om_lexer_scan(source, &lexed[index]);
        if (status == OM_STATUS_OK) {
            status = collect_lex_diagnostics(&diagnostics, source, &lexed[index]);
        }
        if (status != OM_STATUS_OK) {
            goto cleanup;
        }
        if (lexed[index].diagnostic_count != 0) {
            all_syntax_valid = false;
            continue;
        }
        status = om_parser_parse(&lexed[index], &parsed[index]);
        if (status == OM_STATUS_OK) {
            status = collect_parse_diagnostics(&diagnostics, source, &parsed[index]);
        }
        if (status != OM_STATUS_OK) {
            goto cleanup;
        }
        if (parsed[index].diagnostic_count != 0) {
            all_syntax_valid = false;
        }
    }

    if (all_syntax_valid) {
        status = om_name_resolve_project(&project, parsed, module_count, NULL,
                                         &resolution);
        if (status == OM_STATUS_OK) {
            status = collect_resolution_diagnostics(&diagnostics, &project,
                                                    &resolution);
        }
        if (status == OM_STATUS_OK && resolution.diagnostic_count == 0) {
            status = om_type_infer_project(&project, parsed, module_count,
                                           &resolution, &inference);
        }
        if (status == OM_STATUS_OK && resolution.diagnostic_count == 0) {
            status = collect_type_diagnostics(&diagnostics, &project, &inference);
        }
        if (status == OM_STATUS_OK && resolution.diagnostic_count == 0
            && inference.diagnostic_count == 0) {
            status = om_semantic_analyze_project(&project, parsed, module_count,
                                                  &resolution, &inference, &semantic);
        }
        if (status == OM_STATUS_OK && semantic.diagnostics != NULL) {
            status = collect_semantic_diagnostics(&diagnostics, &project, &semantic);
        }
        if (status == OM_STATUS_OK && semantic.diagnostic_count == 0
            && inference.diagnostic_count == 0 && resolution.diagnostic_count == 0) {
            status = om_interface_cache_build(&project, &resolution, &inference,
                                              &semantic, &interfaces);
        }
        semantic_valid = status == OM_STATUS_OK && diagnostics.length == 0;
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: check failed (%s)\n", om_status_name(status));
        goto cleanup;
    }
    if (include_inferred || include_interfaces) {
        if (strcmp(command, "check") != 0 || !json) {
            (void)fprintf(stderr, "ombasic: analysis details require --diagnostics=json\n");
            goto cleanup;
        }
        print_analysis_json(&diagnostics, &project, parsed, module_count,
                            &resolution, &inference, &interfaces,
                            include_inferred, include_interfaces);
    } else if (strcmp(command, "check") == 0 || !semantic_valid) {
        print_diagnostics(&diagnostics, json);
    }
    exit_code = semantic_valid ? 0 : 1;
    if (semantic_valid && strcmp(command, "check") != 0) {
        status = om_bytecode_emit_project(&project, parsed, module_count,
                                          &resolution, &inference, &semantic,
                                          &emitted);
        if (status == OM_STATUS_OK) {
            status = collect_backend_diagnostics(&diagnostics, &project, parsed,
                                                 module_count, &emitted);
        }
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr, "ombasic: bytecode emission failed (%s)\n",
                          om_status_name(status));
            exit_code = 1;
        } else if (diagnostics.length != 0) {
            print_diagnostics(&diagnostics, json);
            exit_code = 1;
        } else if (strcmp(command, "build") == 0) {
            status = om_bytecode_encode(&emitted.image, &encoded);
            if (status != OM_STATUS_OK) {
                (void)fprintf(stderr, "ombasic: bytecode encoding failed (%s)\n",
                              om_status_name(status));
                exit_code = 1;
            } else if (!write_encoded_bytecode(output_path,
                (Om_Const_Byte_Slice){encoded.data, encoded.length})) {
                (void)fprintf(stderr, "ombasic: cannot write '%s'\n", output_path);
                exit_code = 1;
            } else {
                (void)fprintf(stderr, "ombasic: wrote %s\n", output_path);
                exit_code = 0;
            }
        } else {
            exit_code = run_bytecode(&project, &emitted.image,
                                     literal_view(entry_name));
        }
    }

cleanup:
    om_byte_slice_destroy(&encoded);
    om_bytecode_emit_result_destroy(&emitted);
    om_free(entry_source);
    om_free(diagnostics.items);
    om_interface_cache_destroy(&interfaces);
    om_semantic_analysis_result_destroy(&semantic);
    om_type_inference_result_destroy(&inference);
    om_resolution_result_destroy(&resolution);
    if (parsed != NULL) {
        for (size_t index = 0; index < om_module_project_module_count(&project); ++index) {
            om_parser_result_destroy(&parsed[index]);
        }
    }
    if (lexed != NULL) {
        for (size_t index = 0; index < om_module_project_module_count(&project); ++index) {
            om_lexer_result_destroy(&lexed[index]);
        }
    }
    om_free(parsed);
    om_free(lexed);
    om_module_project_destroy(&project);
    om_source_root_destroy(&root);
    return exit_code;
}
