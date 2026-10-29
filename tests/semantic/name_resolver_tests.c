#include "test.h"

#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/source/source_text.h"
#include "ombasic/syntax/lexer.h"

#if !defined(OM_SEMANTIC_TEST_FIXTURE_ROOT)
#error OM_SEMANTIC_TEST_FIXTURE_ROOT must name the checked-in resolver fixture directory
#endif

#define SEMANTIC_ASSERT(expression)                                             \
    do {                                                                        \
        if (!(expression)) {                                                    \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",           \
                          __FILE__, __LINE__, #expression);                     \
            goto cleanup;                                                       \
        }                                                                       \
    } while (false)

typedef struct Om_Semantic_Test_Project {
    Om_Source_Root source_root;
    Om_Module_Project project;
    Om_Lexer_Result *lexed_modules;
    Om_Parser_Result *parsed_modules;
    size_t module_count;
    Om_Resolution_Result resolution;
} Om_Semantic_Test_Project;

static void destroy_test_project(Om_Semantic_Test_Project *test_project)
{
    if (test_project == NULL) {
        return;
    }
    om_resolution_result_destroy(&test_project->resolution);
    for (size_t index = 0; index < test_project->module_count; ++index) {
        if (test_project->parsed_modules != NULL) {
            om_parser_result_destroy(&test_project->parsed_modules[index]);
        }
        if (test_project->lexed_modules != NULL) {
            om_lexer_result_destroy(&test_project->lexed_modules[index]);
        }
    }
    om_free(test_project->parsed_modules);
    om_free(test_project->lexed_modules);
    om_module_project_destroy(&test_project->project);
    om_source_root_destroy(&test_project->source_root);
    *test_project = (Om_Semantic_Test_Project){0};
}

static bool load_and_resolve_with_config(
    const char *entry_path,
    Om_Semantic_Test_Project *test_project,
    const Om_Resolver_Config *resolver_config)
{
    static const char fixture_root[] = OM_SEMANTIC_TEST_FIXTURE_ROOT;
    if (entry_path == NULL || test_project == NULL) {
        return false;
    }
    Om_Status status = om_source_root_init(
        &test_project->source_root,
        (Om_String_View){fixture_root, sizeof(fixture_root) - 1});
    if (status != OM_STATUS_OK) {
        return false;
    }
    status = om_module_project_load(
        &test_project->source_root,
        (Om_String_View){entry_path, strlen(entry_path)},
        4096,
        &test_project->project);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "load %s returned %s\n", entry_path,
                      om_status_name(status));
        return false;
    }
    test_project->module_count = om_module_project_module_count(
        &test_project->project);
    test_project->lexed_modules = om_alloc_array(
        test_project->module_count, sizeof(*test_project->lexed_modules));
    test_project->parsed_modules = om_alloc_array(
        test_project->module_count, sizeof(*test_project->parsed_modules));
    if (test_project->lexed_modules != NULL) {
        (void)memset(test_project->lexed_modules, 0,
                     test_project->module_count
                         * sizeof(*test_project->lexed_modules));
    }
    if (test_project->parsed_modules != NULL) {
        (void)memset(test_project->parsed_modules, 0,
                     test_project->module_count
                         * sizeof(*test_project->parsed_modules));
    }
    if (test_project->module_count == 0 || test_project->lexed_modules == NULL
        || test_project->parsed_modules == NULL) {
        return false;
    }
    for (size_t index = 0; index < test_project->module_count; ++index) {
        const Om_Source_Text *source = om_module_project_source(
            &test_project->project, (Om_Module_Id){(uint32_t)index});
        if (source == NULL
            || om_lexer_scan(source, &test_project->lexed_modules[index])
                != OM_STATUS_OK
            || test_project->lexed_modules[index].diagnostic_count != 0
            || om_parser_parse(&test_project->lexed_modules[index],
                               &test_project->parsed_modules[index])
                != OM_STATUS_OK
            || test_project->parsed_modules[index].diagnostic_count != 0) {
            (void)fprintf(stderr, "lex or parse failed for module %zu\n", index);
            return false;
        }
    }
    status = om_name_resolve_project(
        &test_project->project, test_project->parsed_modules,
        test_project->module_count, resolver_config,
        &test_project->resolution);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "resolve %s returned %s\n", entry_path,
                      om_status_name(status));
        return false;
    }
    return true;
}

static bool load_and_resolve(const char *entry_path,
                             Om_Semantic_Test_Project *test_project)
{
    return load_and_resolve_with_config(entry_path, test_project, NULL);
}

static Om_Module_Id find_module(const Om_Semantic_Test_Project *test_project,
                                const char *path)
{
    if (test_project == NULL || path == NULL) {
        return OM_MODULE_ID_INVALID;
    }
    for (size_t index = 0; index < test_project->module_count; ++index) {
        const Om_Module_Id module = {(uint32_t)index};
        if (om_string_view_equal(
                om_module_graph_path(&test_project->project.graph, module),
                (Om_String_View){path, strlen(path)})) {
            return module;
        }
    }
    return OM_MODULE_ID_INVALID;
}

static Om_Ast_Node_Id find_node(const Om_Ast *ast, Om_Ast_Kind kind,
                                const char *name)
{
    if (ast == NULL || name == NULL) {
        return OM_AST_NODE_ID_INVALID;
    }
    const Om_String_View expected = {name, strlen(name)};
    for (size_t index = 0; index < ast->node_count; ++index) {
        const Om_Ast_Node_Id node_id = {(uint32_t)index};
        const Om_Ast_Node *node = om_ast_get_node(ast, node_id);
        if (node != NULL && node->kind == kind
            && om_string_view_equal(node->name, expected)) {
            return node_id;
        }
    }
    return OM_AST_NODE_ID_INVALID;
}

static size_t count_diagnostics(const Om_Resolution_Result *resolution,
                                Om_Resolution_Diagnostic_Code code)
{
    size_t count = 0;
    if (resolution == NULL) {
        return 0;
    }
    for (size_t index = 0; index < resolution->diagnostic_count; ++index) {
        if (resolution->diagnostics[index].code == code) {
            ++count;
        }
    }
    return count;
}

static size_t count_type_diagnostics(const Om_Type_Inference_Result *inference,
                                    Om_Type_Inference_Diagnostic_Code code)
{
    size_t count = 0;
    if (inference == NULL) {
        return 0;
    }
    for (size_t index = 0; index < inference->diagnostic_count; ++index) {
        if (inference->diagnostics[index].code == code) {
            ++count;
        }
    }
    return count;
}

static size_t count_semantic_diagnostics(
    const Om_Semantic_Analysis_Result *analysis,
    Om_Semantic_Analysis_Diagnostic_Code code)
{
    size_t count = 0;
    if (analysis == NULL) {
        return 0;
    }
    for (size_t index = 0; index < analysis->diagnostic_count; ++index) {
        if (analysis->diagnostics[index].code == code) {
            ++count;
        }
    }
    return count;
}

static bool has_semantic_diagnostic_at(
    const Om_Semantic_Analysis_Result *analysis,
    Om_Semantic_Analysis_Diagnostic_Code code,
    Om_Module_Id module,
    Om_Source_Span span)
{
    if (analysis == NULL) {
        return false;
    }
    for (size_t index = 0; index < analysis->diagnostic_count; ++index) {
        const Om_Semantic_Analysis_Diagnostic *diagnostic =
            &analysis->diagnostics[index];
        if (diagnostic->code == code
            && diagnostic->module_id.value == module.value
            && diagnostic->span.source.value == span.source.value
            && diagnostic->span.start == span.start
            && diagnostic->span.end == span.end) {
            return true;
        }
    }
    return false;
}

static bool infer_and_analyze(Om_Semantic_Test_Project *test_project,
                              Om_Type_Inference_Result *inference,
                              Om_Semantic_Analysis_Result *analysis)
{
    if (test_project == NULL || inference == NULL || analysis == NULL) {
        return false;
    }
    Om_Status status = om_type_infer_project(
        &test_project->project, test_project->parsed_modules,
        test_project->module_count, &test_project->resolution, inference);
    if (status != OM_STATUS_OK || inference->diagnostic_count != 0) {
        (void)fprintf(stderr, "type inference returned %s with %zu diagnostics\n",
                      om_status_name(status), inference->diagnostic_count);
        return false;
    }
    status = om_semantic_analyze_project(
        &test_project->project, test_project->parsed_modules,
        test_project->module_count, &test_project->resolution, inference,
        analysis);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "semantic analysis returned %s\n",
                      om_status_name(status));
    }
    return status == OM_STATUS_OK;
}

static bool inferred_node_has_kind(const Om_Type_Inference_Result *inference,
                                   Om_Module_Id module,
                                   Om_Ast_Node_Id node,
                                   Om_Type_Kind expected_kind)
{
    const Om_Type_Id type_id = om_type_inference_type_for_node(
        inference, module, node);
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    return type_id.value != UINT32_MAX
        && om_type_store_resolve(&inference->type_store, type_id, &resolved)
           == OM_STATUS_OK
        && om_type_store_get(&inference->type_store, resolved, &view)
           == OM_STATUS_OK
        && view.kind == expected_kind;
}

bool om_test_type_inference_project(void)
{
    Om_Semantic_Test_Project test_project = {0};
    Om_Type_Inference_Result inference = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("type_inference/positive.bas",
                                     &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 0);
    SEMANTIC_ASSERT(om_type_infer_project(
                        &test_project.project, test_project.parsed_modules,
                        test_project.module_count, &test_project.resolution,
                        &inference) == OM_STATUS_OK);
    SEMANTIC_ASSERT(inference.diagnostic_count == 0);
    const Om_Module_Id module = find_module(&test_project,
                                            "type_inference/positive.bas");
    const Om_Ast *ast = &test_project.parsed_modules[module.value].ast;
    const Om_Ast_Node_Id identity_id = find_node(
        ast, OM_AST_FUNCTION_DECLARATION, "identity");
    SEMANTIC_ASSERT(identity_id.value != UINT32_MAX);
    const Om_Symbol_Id identity_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, module, identity_id);
    const Om_Type_Scheme *identity_scheme =
        om_type_inference_scheme_for_symbol(&inference, identity_symbol);
    SEMANTIC_ASSERT(identity_scheme != NULL
                    && identity_scheme->quantified_variable_count == 1);

    size_t int_identity_calls = 0;
    size_t string_identity_calls = 0;
    for (size_t index = 0; index < ast->node_count; ++index) {
        const Om_Ast_Node_Id call_id = {(uint32_t)index};
        const Om_Ast_Node *call = om_ast_get_node(ast, call_id);
        if (call == NULL || call->kind != OM_AST_CALL || call->child_count != 2) {
            continue;
        }
        const Om_Ast_Node *callee = om_ast_get_node(
            ast, om_ast_get_child(ast, call, 0));
        if (callee == NULL || callee->kind != OM_AST_NAME
            || !om_string_view_equal(callee->name,
                                     (Om_String_View){"identity", 8})) {
            continue;
        }
        const Om_Ast_Node *argument = om_ast_get_node(
            ast, om_ast_get_child(ast, call, 1));
        if (argument != NULL && argument->kind == OM_AST_INTEGER_LITERAL) {
            SEMANTIC_ASSERT(inferred_node_has_kind(&inference, module, call_id,
                                                   OM_TYPE_INT));
            ++int_identity_calls;
        } else if (argument != NULL && argument->kind == OM_AST_STRING_LITERAL) {
            SEMANTIC_ASSERT(inferred_node_has_kind(&inference, module, call_id,
                                                   OM_TYPE_STRING));
            ++string_identity_calls;
        }
    }
    SEMANTIC_ASSERT(int_identity_calls == 1 && string_identity_calls == 1);
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);

    SEMANTIC_ASSERT(load_and_resolve("type_inference/mismatch.bas",
                                     &test_project));
    SEMANTIC_ASSERT(om_type_infer_project(
                        &test_project.project, test_project.parsed_modules,
                        test_project.module_count, &test_project.resolution,
                        &inference) == OM_STATUS_OK);
    SEMANTIC_ASSERT(count_type_diagnostics(
                        &inference,
                        OM_TYPE_INFERENCE_DIAGNOSTIC_E0401) == 4);
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);

    SEMANTIC_ASSERT(load_and_resolve("type_inference/empty_collection.bas",
                                     &test_project));
    SEMANTIC_ASSERT(om_type_infer_project(
                        &test_project.project, test_project.parsed_modules,
                        test_project.module_count, &test_project.resolution,
                        &inference) == OM_STATUS_OK);
    SEMANTIC_ASSERT(count_type_diagnostics(
                        &inference,
                        OM_TYPE_INFERENCE_DIAGNOSTIC_E0402) == 1);
    passed = true;

cleanup:
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_semantic_constraints(void)
{
    Om_Semantic_Test_Project test_project = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result analysis = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("type_inference/positive.bas",
                                     &test_project));
    SEMANTIC_ASSERT(infer_and_analyze(&test_project, &inference, &analysis));
    SEMANTIC_ASSERT(analysis.diagnostic_count == 0);
    const Om_Module_Id positive_module = find_module(
        &test_project, "type_inference/positive.bas");
    const Om_Ast *positive_ast =
        &test_project.parsed_modules[positive_module.value].ast;
    const Om_Ast_Node_Id worker_node = find_node(
        positive_ast, OM_AST_ACTOR_DECLARATION, "Worker");
    const Om_Ast_Node_Id ping_node = find_node(
        positive_ast, OM_AST_MESSAGE_DECLARATION, "Ping");
    const Om_Ast_Node_Id main_node = find_node(
        positive_ast, OM_AST_ACTION_DECLARATION, "main");
    const Om_Ast_Node_Id state_read = find_node(
        positive_ast, OM_AST_NAME, "count");
    SEMANTIC_ASSERT(worker_node.value != UINT32_MAX
                    && ping_node.value != UINT32_MAX
                    && main_node.value != UINT32_MAX
                    && state_read.value != UINT32_MAX);
    const Om_Symbol_Id worker_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, positive_module, worker_node);
    const Om_Symbol_Id ping_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, positive_module, ping_node);
    const Om_Ast_Node_Id pair_message_node = find_node(
        positive_ast, OM_AST_MESSAGE_DECLARATION, "Pair_Message");
    SEMANTIC_ASSERT(pair_message_node.value != UINT32_MAX);
    const Om_Symbol_Id pair_message_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, positive_module, pair_message_node);
    const Om_Symbol_Id main_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, positive_module, main_node);
    SEMANTIC_ASSERT(om_semantic_actor_accepts(&analysis, worker_symbol,
                                              ping_symbol));
    SEMANTIC_ASSERT(om_semantic_actor_accepts(&analysis, worker_symbol,
                                              pair_message_symbol));
    Om_Symbol_Id actor_down_symbol = OM_SYMBOL_ID_INVALID;
    for (size_t index = 0; index < test_project.resolution.symbol_count; ++index) {
        const Om_Symbol *symbol = &test_project.resolution.symbols[index];
        if (symbol->kind == OM_SYMBOL_BUILTIN
            && om_string_view_equal(symbol->name,
                                    (Om_String_View){"Actor_Down", 10})) {
            actor_down_symbol = symbol->id;
            break;
        }
    }
    SEMANTIC_ASSERT(actor_down_symbol.value != UINT32_MAX
                    && om_semantic_actor_accepts(&analysis, worker_symbol,
                                                 actor_down_symbol));
    SEMANTIC_ASSERT((om_semantic_effect_for_symbol(&analysis, main_symbol)
                     & ((uint32_t)OM_EFFECT_SPAWN_ACTOR
                        | (uint32_t)OM_EFFECT_SEND_MESSAGE))
                    == ((uint32_t)OM_EFFECT_SPAWN_ACTOR
                        | (uint32_t)OM_EFFECT_SEND_MESSAGE));
    SEMANTIC_ASSERT((om_semantic_effect_for_node(&analysis, positive_module,
                                                 state_read)
                     & (uint32_t)OM_EFFECT_READS_ACTOR_STATE) != 0);
    bool serializable = false;
    const Om_Ast_Node *pair_send = NULL;
    Om_Ast_Node_Id message_node = OM_AST_NODE_ID_INVALID;
    for (size_t index = 0; index < positive_ast->node_count; ++index) {
        const Om_Ast_Node *candidate = om_ast_get_node(
            positive_ast, (Om_Ast_Node_Id){(uint32_t)index});
        if (candidate == NULL || candidate->kind != OM_AST_SEND
            || candidate->child_count != 2) {
            continue;
        }
        const Om_Ast_Node_Id candidate_message = om_ast_get_child(
            positive_ast, candidate, 1);
        const Om_Ast_Node *candidate_call = om_ast_get_node(
            positive_ast, candidate_message);
        if (candidate_call == NULL || candidate_call->kind != OM_AST_CALL
            || candidate_call->child_count == 0) {
            continue;
        }
        const Om_Ast_Node *callee = om_ast_get_node(
            positive_ast, om_ast_get_child(positive_ast, candidate_call, 0));
        if (callee != NULL && callee->kind == OM_AST_NAME
            && om_string_view_equal(callee->name,
                                    (Om_String_View){"Pair_Message", 12})) {
            pair_send = candidate;
            message_node = candidate_message;
            break;
        }
    }
    SEMANTIC_ASSERT(pair_send != NULL
                    && message_node.value != UINT32_MAX);
    const Om_Type_Id message_type = om_type_inference_type_for_node(
        &inference, positive_module, message_node);
    SEMANTIC_ASSERT(om_semantic_type_is_serializable(
                        &analysis, message_type, &serializable)
                    && serializable);
    om_semantic_analysis_result_destroy(&analysis);
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);

    SEMANTIC_ASSERT(load_and_resolve("semantic_constraints/effects.bas",
                                     &test_project));
    SEMANTIC_ASSERT(infer_and_analyze(&test_project, &inference, &analysis));
    SEMANTIC_ASSERT(count_semantic_diagnostics(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501) == 3);
    const Om_Module_Id effects_module = find_module(
        &test_project, "semantic_constraints/effects.bas");
    const Om_Ast *effects_ast =
        &test_project.parsed_modules[effects_module.value].ast;
    const Om_Ast_Node_Id effectful_function = find_node(
        effects_ast, OM_AST_FUNCTION_DECLARATION, "hidden_effect");
    const Om_Ast_Node *effectful_function_node = om_ast_get_node(
        effects_ast, effectful_function);
    SEMANTIC_ASSERT(effectful_function_node != NULL
                    && has_semantic_diagnostic_at(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501,
                        effects_module, effectful_function_node->span));
    bool watch_diagnosed = false;
    bool stop_diagnosed = false;
    for (size_t index = 0; index < effects_ast->node_count; ++index) {
        const Om_Ast_Node_Id node_id = {(uint32_t)index};
        const Om_Ast_Node *node = om_ast_get_node(effects_ast, node_id);
        if (node == NULL) {
            continue;
        }
        if (node->kind == OM_AST_WATCH) {
            watch_diagnosed = has_semantic_diagnostic_at(
                &analysis, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501,
                effects_module, node->span);
        } else if (node->kind == OM_AST_STOP) {
            stop_diagnosed = has_semantic_diagnostic_at(
                &analysis, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501,
                effects_module, node->span);
        }
    }
    SEMANTIC_ASSERT(watch_diagnosed && stop_diagnosed);
    om_semantic_analysis_result_destroy(&analysis);
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);

    const Om_String_View native_prelude[] = {{"native_call", 11}};
    const Om_Resolver_Config resolver_config = {
        .prelude_names = native_prelude,
        .prelude_name_count = sizeof(native_prelude) / sizeof(native_prelude[0]),
    };
    SEMANTIC_ASSERT(load_and_resolve_with_config(
        "semantic_constraints/native_effect.bas", &test_project,
        &resolver_config));
    SEMANTIC_ASSERT(infer_and_analyze(&test_project, &inference, &analysis));
    SEMANTIC_ASSERT(count_semantic_diagnostics(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501) == 2);
    const Om_Module_Id native_module = find_module(
        &test_project, "semantic_constraints/native_effect.bas");
    const Om_Ast *native_ast =
        &test_project.parsed_modules[native_module.value].ast;
    const Om_Ast_Node_Id wrapper_node = find_node(
        native_ast, OM_AST_FUNCTION_DECLARATION, "native_wrapper");
    const Om_Ast_Node_Id hidden_node = find_node(
        native_ast, OM_AST_FUNCTION_DECLARATION, "hidden_effect");
    SEMANTIC_ASSERT(wrapper_node.value != UINT32_MAX
                    && hidden_node.value != UINT32_MAX);
    const Om_Symbol_Id hidden_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, native_module, hidden_node);
    SEMANTIC_ASSERT((om_semantic_effect_for_symbol(&analysis, hidden_symbol)
                     & (uint32_t)OM_EFFECT_NATIVE) != 0);
    om_semantic_analysis_result_destroy(&analysis);
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);

    SEMANTIC_ASSERT(load_and_resolve("semantic_constraints/wrong_message.bas",
                                     &test_project));
    SEMANTIC_ASSERT(infer_and_analyze(&test_project, &inference, &analysis));
    SEMANTIC_ASSERT(count_semantic_diagnostics(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601) == 1);
    SEMANTIC_ASSERT(count_semantic_diagnostics(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701) == 0);
    const Om_Module_Id wrong_message_module = find_module(
        &test_project, "semantic_constraints/wrong_message.bas");
    const Om_Ast *wrong_message_ast =
        &test_project.parsed_modules[wrong_message_module.value].ast;
    const Om_Ast_Node_Id wrong_send_id = find_node(
        wrong_message_ast, OM_AST_SEND, "");
    const Om_Ast_Node *wrong_send = wrong_send_id.value != UINT32_MAX
        ? om_ast_get_node(wrong_message_ast, wrong_send_id) : NULL;
    SEMANTIC_ASSERT(wrong_send != NULL
                    && has_semantic_diagnostic_at(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601,
                        wrong_message_module, wrong_send->span));
    om_semantic_analysis_result_destroy(&analysis);
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);

    SEMANTIC_ASSERT(load_and_resolve(
                        "semantic_constraints/nonserializable.bas",
                        &test_project));
    SEMANTIC_ASSERT(infer_and_analyze(&test_project, &inference, &analysis));
    SEMANTIC_ASSERT(count_semantic_diagnostics(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601) == 0);
    SEMANTIC_ASSERT(count_semantic_diagnostics(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701) == 1);
    const Om_Module_Id nonserializable_module = find_module(
        &test_project, "semantic_constraints/nonserializable.bas");
    const Om_Ast *nonserializable_ast =
        &test_project.parsed_modules[nonserializable_module.value].ast;
    const Om_Ast_Node_Id bad_send_id = find_node(
        nonserializable_ast, OM_AST_SEND, "");
    const Om_Ast_Node *bad_send = bad_send_id.value != UINT32_MAX
        ? om_ast_get_node(nonserializable_ast, bad_send_id) : NULL;
    const Om_Ast_Node_Id bad_message_id = bad_send != NULL
        ? om_ast_get_child(nonserializable_ast, bad_send, 1)
        : OM_AST_NODE_ID_INVALID;
    const Om_Ast_Node *bad_message = bad_message_id.value != UINT32_MAX
        ? om_ast_get_node(nonserializable_ast, bad_message_id) : NULL;
    SEMANTIC_ASSERT(bad_message != NULL
                    && has_semantic_diagnostic_at(
                        &analysis,
                        OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701,
                        nonserializable_module, bad_message->span));
    passed = true;

cleanup:
    om_semantic_analysis_result_destroy(&analysis);
    om_type_inference_result_destroy(&inference);
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_visibility(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("visibility/main.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 0);
    const Om_Module_Id main_module = find_module(&test_project, "visibility/main.bas");
    const Om_Module_Id database_module = find_module(
        &test_project, "visibility/storage/database.bas");
    SEMANTIC_ASSERT(main_module.value == 0 && database_module.value != UINT32_MAX);
    const Om_Ast *main_ast = &test_project.parsed_modules[main_module.value].ast;
    const Om_Ast_Node_Id member_id = find_node(main_ast, OM_AST_MEMBER, "open");
    SEMANTIC_ASSERT(member_id.value != UINT32_MAX);
    const Om_Ast_Node *member = om_ast_get_node(main_ast, member_id);
    const Om_Ast_Node_Id receiver_id = om_ast_get_child(main_ast, member, 0);
    const Om_Symbol_Id receiver_symbol_id = om_resolution_symbol_for_node(
        &test_project.resolution, main_module, receiver_id);
    const Om_Symbol *receiver_symbol = om_resolution_symbol_by_id(
        &test_project.resolution, receiver_symbol_id);
    SEMANTIC_ASSERT(receiver_symbol != NULL
                    && receiver_symbol->kind == OM_SYMBOL_MODULE_ALIAS
                    && receiver_symbol->target_module.value == database_module.value);
    const Om_Symbol_Id member_symbol_id = om_resolution_symbol_for_node(
        &test_project.resolution, main_module, member_id);
    const Om_Symbol *member_symbol = om_resolution_symbol_by_id(
        &test_project.resolution, member_symbol_id);
    SEMANTIC_ASSERT(member_symbol != NULL
                    && member_symbol->kind == OM_SYMBOL_FUNCTION
                    && member_symbol->module_id.value == database_module.value
                    && member_symbol->exported);
    const Om_Scope_Id member_scope = om_resolution_scope_for_node(
        &test_project.resolution, main_module, member_id);
    SEMANTIC_ASSERT(member_scope.value < test_project.resolution.scope_count
                    && test_project.resolution.scopes[member_scope.value].kind
                       == OM_SCOPE_BLOCK);
    const Om_Ast_Node_Id root_id = main_ast->root;
    const Om_Scope_Id module_scope = om_resolution_scope_for_node(
        &test_project.resolution, main_module, root_id);
    SEMANTIC_ASSERT(module_scope.value < test_project.resolution.scope_count
                    && test_project.resolution.scopes[module_scope.value].kind
                       == OM_SCOPE_MODULE);
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_private_member(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("visibility/private_main.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 1);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0302) == 1);
    const Om_Resolution_Diagnostic *diagnostic =
        &test_project.resolution.diagnostics[0];
    SEMANTIC_ASSERT(diagnostic->module_id.value == 0
                    && diagnostic->has_related_span);
    const Om_Source_Text *source = om_module_project_source(
        &test_project.project, diagnostic->module_id);
    Om_Const_Byte_Slice selected = {0};
    SEMANTIC_ASSERT(source != NULL
                    && om_source_text_slice(source, diagnostic->span, &selected)
                       == OM_STATUS_OK
                    && selected.length == sizeof("secret") - 1
                    && memcmp(selected.data, "secret", selected.length) == 0);
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_alias_collision(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("alias_collision/main.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 1);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0306) == 1);
    SEMANTIC_ASSERT(test_project.resolution.diagnostics[0].module_id.value == 0);
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_declaration_diagnostics(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("declarations/collisions.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 3);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0303) == 1);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0304) == 1);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0305) == 1);
    for (size_t index = 0; index < test_project.resolution.diagnostic_count; ++index) {
        SEMANTIC_ASSERT(test_project.resolution.diagnostics[index].has_related_span);
    }
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_bindings_and_scopes(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("scopes/shadow.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 2);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0305) == 1);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0307) == 1);
    destroy_test_project(&test_project);

    SEMANTIC_ASSERT(load_and_resolve("scopes/siblings.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 0);
    const Om_Ast *ast = &test_project.parsed_modules[0].ast;
    Om_Ast_Node_Id values[2] = {OM_AST_NODE_ID_INVALID, OM_AST_NODE_ID_INVALID};
    size_t value_count = 0;
    for (size_t index = 0; index < ast->node_count; ++index) {
        const Om_Ast_Node_Id node_id = {(uint32_t)index};
        const Om_Ast_Node *node = om_ast_get_node(ast, node_id);
        if (node != NULL && node->kind == OM_AST_BINDING
            && om_string_view_equal(node->name, (Om_String_View){"value", 5})) {
            if (value_count < 2) {
                values[value_count] = node_id;
            }
            ++value_count;
        }
    }
    SEMANTIC_ASSERT(value_count == 2);
    const Om_Symbol_Id first_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, (Om_Module_Id){0}, values[0]);
    const Om_Symbol_Id second_symbol = om_resolution_symbol_for_node(
        &test_project.resolution, (Om_Module_Id){0}, values[1]);
    const Om_Symbol *first = om_resolution_symbol_by_id(
        &test_project.resolution, first_symbol);
    const Om_Symbol *second = om_resolution_symbol_by_id(
        &test_project.resolution, second_symbol);
    SEMANTIC_ASSERT(first != NULL && second != NULL
                    && first->id.value != second->id.value
                    && first->scope_id.value != second->scope_id.value);
    const Om_Scope_Id first_scope = om_resolution_scope_for_node(
        &test_project.resolution, (Om_Module_Id){0}, values[0]);
    const Om_Scope_Id second_scope = om_resolution_scope_for_node(
        &test_project.resolution, (Om_Module_Id){0}, values[1]);
    SEMANTIC_ASSERT(first_scope.value == first->scope_id.value
                    && second_scope.value == second->scope_id.value);
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_undefined_name(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("lookup/undefined.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 1);
    SEMANTIC_ASSERT(count_diagnostics(
                        &test_project.resolution,
                        OM_RESOLUTION_DIAGNOSTIC_E0301) == 1);
    const Om_Source_Text *source = om_module_project_source(
        &test_project.project, (Om_Module_Id){0});
    Om_Const_Byte_Slice selected = {0};
    SEMANTIC_ASSERT(source != NULL
                    && om_source_text_slice(source,
                                            test_project.resolution.diagnostics[0].span,
                                            &selected) == OM_STATUS_OK
                    && selected.length == sizeof("missing") - 1
                    && memcmp(selected.data, "missing", selected.length) == 0);
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_patterns_loops_and_actor(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("scopes/patterns.bas", &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 0);
    const Om_Ast *ast = &test_project.parsed_modules[0].ast;
    bool found_case_binding = false;
    for (size_t index = 0; index < ast->node_count; ++index) {
        const Om_Ast_Node_Id node_id = {(uint32_t)index};
        const Om_Ast_Node *node = om_ast_get_node(ast, node_id);
        if (node == NULL || node->kind != OM_AST_NAME
            || !om_string_view_equal(node->name, (Om_String_View){"value", 5})) {
            continue;
        }
        const Om_Symbol_Id binding_symbol_id = om_resolution_symbol_for_node(
            &test_project.resolution, (Om_Module_Id){0}, node_id);
        const Om_Symbol *binding_symbol = om_resolution_symbol_by_id(
            &test_project.resolution, binding_symbol_id);
        if (binding_symbol != NULL
            && binding_symbol->kind == OM_SYMBOL_IMMUTABLE_BINDING) {
            found_case_binding = true;
        }
    }
    SEMANTIC_ASSERT(found_case_binding);

    const Om_Ast_Node_Id actor_id = find_node(ast, OM_AST_ACTOR_DECLARATION,
                                              "Worker");
    const Om_Ast_Node_Id spawn_id = find_node(ast, OM_AST_SPAWN, "Worker");
    SEMANTIC_ASSERT(actor_id.value != UINT32_MAX && spawn_id.value != UINT32_MAX);
    const Om_Symbol_Id actor_symbol_id = om_resolution_symbol_for_node(
        &test_project.resolution, (Om_Module_Id){0}, spawn_id);
    const Om_Symbol *actor_symbol = om_resolution_symbol_by_id(
        &test_project.resolution, actor_symbol_id);
    SEMANTIC_ASSERT(actor_symbol != NULL
                    && actor_symbol->kind == OM_SYMBOL_ACTOR
                    && actor_symbol->declaration_node.value == actor_id.value);
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_name_resolution_builtin_identities(void)
{
    Om_Semantic_Test_Project test_project = {0};
    bool passed = false;
    SEMANTIC_ASSERT(load_and_resolve("builtin_identity/main.bas",
                                     &test_project));
    SEMANTIC_ASSERT(test_project.resolution.diagnostic_count == 0);
    const Om_Module_Id module = find_module(&test_project,
                                            "builtin_identity/main.bas");
    const Om_Ast *ast = &test_project.parsed_modules[module.value].ast;
    bool found_actor_down_reason = false;
    bool found_ok = false;
    bool found_error = false;
    bool found_actor_down = false;
    for (size_t index = 0; index < test_project.resolution.symbol_count; ++index) {
        const Om_Symbol *symbol = &test_project.resolution.symbols[index];
        if (symbol->kind != OM_SYMBOL_BUILTIN) {
            continue;
        }
        found_actor_down_reason = found_actor_down_reason
            || symbol->builtin_id == OM_BUILTIN_ACTOR_DOWN_REASON;
        found_ok = found_ok || symbol->builtin_id == OM_BUILTIN_OK;
        found_error = found_error || symbol->builtin_id == OM_BUILTIN_ERROR;
        found_actor_down = found_actor_down
            || symbol->builtin_id == OM_BUILTIN_ACTOR_DOWN;
    }
    SEMANTIC_ASSERT(found_actor_down_reason && found_ok && found_error
                    && found_actor_down);
    const struct {
        const char *name;
        Om_Builtin_Member_Id id;
    } members[] = {
        {"normal", OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NORMAL},
        {"failed", OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_FAILED},
        {"node_unreachable",
         OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NODE_UNREACHABLE},
    };
    for (size_t index = 0; index < sizeof(members) / sizeof(members[0]); ++index) {
        const Om_Ast_Node_Id member = find_node(ast, OM_AST_MEMBER,
                                                members[index].name);
        SEMANTIC_ASSERT(member.value != UINT32_MAX
                        && om_resolution_builtin_member_for_node(
                               &test_project.resolution, module, member)
                            == members[index].id);
    }
    passed = true;

cleanup:
    destroy_test_project(&test_project);
    OM_TEST_ASSERT(passed);
    return true;
}
