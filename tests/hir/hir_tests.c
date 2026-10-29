#include "ombasic/ir/hir.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/ir/specialize.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/native/metadata/omi.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/syntax/lexer.h"
#include "ombasic/syntax/parser.h"

#if !defined(OM_HIR_TEST_FIXTURE_ROOT)
#error OM_HIR_TEST_FIXTURE_ROOT must name the semantic fixture directory
#endif
#if !defined(OM_HIR_TEST_GOLDEN_ROOT)
#error OM_HIR_TEST_GOLDEN_ROOT must name the normalized HIR snapshot directory
#endif

#define HIR_ASSERT(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",             \
                          __FILE__, __LINE__, #expression);                      \
            goto cleanup;                                                        \
        }                                                                        \
    } while (false)

static bool contains_text(Om_String_View text, const char *needle)
{
    const size_t needle_length = strlen(needle);
    if (needle_length > text.length) {
        return false;
    }
    for (size_t index = 0; index <= text.length - needle_length; ++index) {
        if (memcmp(text.data + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_hir_diagnostic(const Om_Hir_Verification_Result *verification,
                               Om_Hir_Diagnostic_Code code)
{
    for (size_t index = 0; index < verification->diagnostic_count; ++index) {
        if (verification->diagnostics[index].code == code) {
            return true;
        }
    }
    return false;
}

static bool update_hir_goldens;

static bool read_file(const char *path, char **data, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || data == NULL || length == NULL) {
        if (file != NULL) (void)fclose(file);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return false;
    }
    const long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    const size_t file_length = (size_t)end;
    char *buffer = om_alloc_array(file_length + 1, sizeof(*buffer));
    if (buffer == NULL) {
        (void)fclose(file);
        return false;
    }
    const bool read_all = fread(buffer, 1, file_length, file) == file_length;
    const bool closed = fclose(file) == 0;
    if (!read_all || !closed) {
        om_free(buffer);
        return false;
    }
    buffer[file_length] = '\0';
    *data = buffer;
    *length = file_length;
    return true;
}

static bool write_file(const char *path, Om_String_View text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const bool wrote_all = fwrite(text.data, 1, text.length, file) == text.length;
    const bool closed = fclose(file) == 0;
    return wrote_all && closed;
}

static bool compare_or_update_golden(const char *golden_name,
                                     Om_String_View actual)
{
    char path[4096];
    const int path_length = snprintf(path, sizeof(path), "%s/%s",
        OM_HIR_TEST_GOLDEN_ROOT, golden_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return false;
    }
    if (update_hir_goldens) {
        return write_file(path, actual);
    }
    char *expected = NULL;
    size_t expected_length = 0;
    const bool loaded = read_file(path, &expected, &expected_length);
    const bool matches = loaded && expected_length == actual.length
        && memcmp(expected, actual.data, actual.length) == 0;
    om_free(expected);
    return matches;
}

static bool test_fixture_has_deterministic_hir(const char *entry_path,
                                               size_t expected_module_count,
                                               const char *golden_name,
                                               const Om_Resolver_Config *resolver_config)
{
    static const char root_path[] = OM_HIR_TEST_FIXTURE_ROOT;
    Om_Source_Root root = {0};
    Om_Module_Project source_project = {0};
    Om_Lexer_Result *lexed = NULL;
    Om_Parser_Result *parsed = NULL;
    Om_Resolution_Result resolution = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result semantic = {0};
    Om_Hir_Project hir = {0};
    Om_Hir_Verification_Result verification = {0};
    Om_Hir_Text first_dump = {0};
    Om_Hir_Text second_dump = {0};
    bool passed = false;

    HIR_ASSERT(entry_path != NULL);
    HIR_ASSERT(om_source_root_init(&root,
        (Om_String_View){root_path, sizeof(root_path) - 1}) == OM_STATUS_OK);
    HIR_ASSERT(om_module_project_load(&root,
        (Om_String_View){entry_path, strlen(entry_path)}, 65536,
        &source_project) == OM_STATUS_OK);
    const size_t module_count = om_module_project_module_count(&source_project);
    HIR_ASSERT(module_count == expected_module_count);
    lexed = om_alloc_array(module_count, sizeof(*lexed));
    parsed = om_alloc_array(module_count, sizeof(*parsed));
    HIR_ASSERT(lexed != NULL && parsed != NULL);
    (void)memset(lexed, 0, module_count * sizeof(*lexed));
    (void)memset(parsed, 0, module_count * sizeof(*parsed));
    for (size_t index = 0; index < module_count; ++index) {
        const Om_Source_Text *source = om_module_project_source(
            &source_project, (Om_Module_Id){(uint32_t)index});
        HIR_ASSERT(source != NULL);
        HIR_ASSERT(om_lexer_scan(source, &lexed[index]) == OM_STATUS_OK);
        HIR_ASSERT(lexed[index].diagnostic_count == 0);
        HIR_ASSERT(om_parser_parse(&lexed[index], &parsed[index])
                   == OM_STATUS_OK);
        HIR_ASSERT(parsed[index].diagnostic_count == 0);
    }
    HIR_ASSERT(om_name_resolve_project(&source_project, parsed, module_count,
        resolver_config, &resolution) == OM_STATUS_OK);
    for (size_t index = 0; index < resolution.diagnostic_count; ++index) {
        (void)fprintf(stderr, "%s: resolution diagnostic %d at module %u span %u-%u\n",
            entry_path, (int)resolution.diagnostics[index].code,
            resolution.diagnostics[index].module_id.value,
            resolution.diagnostics[index].span.start,
            resolution.diagnostics[index].span.end);
    }
    HIR_ASSERT(resolution.diagnostic_count == 0);
    HIR_ASSERT(om_type_infer_project(&source_project, parsed, module_count,
        &resolution, &inference) == OM_STATUS_OK);
    HIR_ASSERT(inference.diagnostic_count == 0);
    HIR_ASSERT(om_semantic_analyze_project(&source_project, parsed, module_count,
        &resolution, &inference, &semantic) == OM_STATUS_OK);
    HIR_ASSERT(semantic.diagnostic_count == 0);
    HIR_ASSERT(om_hir_lower_project(&source_project, parsed, module_count,
        &resolution, &inference, &semantic, &hir) == OM_STATUS_OK);
    HIR_ASSERT(hir.module_count == module_count && hir.node_count > 0);
    HIR_ASSERT(om_hir_verify_project(&hir, &verification) == OM_STATUS_OK);
    for (size_t index = 0; index < verification.diagnostic_count; ++index) {
        const Om_Hir_Node *node = om_hir_get_node(
            &hir, verification.diagnostics[index].node_id);
        (void)fprintf(stderr, "%s: HIR %s at node %u\n", entry_path,
            om_hir_diagnostic_code_name(verification.diagnostics[index].code),
            verification.diagnostics[index].node_id.value);
        if (node != NULL) {
            (void)fprintf(stderr, "  kind=%s type=%u symbol=%u source=%u\n",
                om_hir_kind_name(node->kind), node->type_id.value,
                node->symbol_id.value, node->source_node.value);
        }
    }
    HIR_ASSERT(verification.diagnostic_count == 0);
    HIR_ASSERT(om_hir_dump(&hir, &first_dump) == OM_STATUS_OK);
    HIR_ASSERT(om_hir_dump(&hir, &second_dump) == OM_STATUS_OK);
    HIR_ASSERT(first_dump.length == second_dump.length
        && memcmp(first_dump.data, second_dump.data, first_dump.length) == 0);
    HIR_ASSERT(compare_or_update_golden(golden_name,
        (Om_String_View){first_dump.data, first_dump.length}));
    passed = true;

cleanup:
    om_hir_text_destroy(&second_dump);
    om_hir_text_destroy(&first_dump);
    om_hir_verification_result_destroy(&verification);
    om_hir_project_destroy(&hir);
    om_semantic_analysis_result_destroy(&semantic);
    om_type_inference_result_destroy(&inference);
    om_resolution_result_destroy(&resolution);
    if (parsed != NULL) {
        const size_t count = om_module_project_module_count(&source_project);
        for (size_t index = 0; index < count; ++index) {
            om_parser_result_destroy(&parsed[index]);
        }
    }
    if (lexed != NULL) {
        const size_t count = om_module_project_module_count(&source_project);
        for (size_t index = 0; index < count; ++index) {
            om_lexer_result_destroy(&lexed[index]);
        }
    }
    om_free(parsed);
    om_free(lexed);
    om_module_project_destroy(&source_project);
    om_source_root_destroy(&root);
    return passed;
}

static bool test_native_declaration_hir(void)
{
    static const char root_path[] = OM_HIR_TEST_FIXTURE_ROOT;
    static const char metadata_relative_path[] = "native_decl/math.omi";
    char metadata_path[4096];
    char *metadata_bytes = NULL;
    size_t metadata_length = 0;
    Om_Source_Text metadata_source = {0};
    Om_Omi_Module metadata_module = {0};
    bool passed = false;

    const int path_length = snprintf(metadata_path, sizeof(metadata_path),
        "%s/%s", root_path, metadata_relative_path);
    HIR_ASSERT(path_length >= 0 && (size_t)path_length < sizeof(metadata_path));
    HIR_ASSERT(read_file(metadata_path, &metadata_bytes, &metadata_length));
    HIR_ASSERT(om_source_text_init(&metadata_source, (Om_Source_Id){91},
        (Om_String_View){metadata_path, (size_t)path_length},
        (Om_Const_Byte_Slice){(const uint8_t *)metadata_bytes, metadata_length},
        65536) == OM_STATUS_OK);
    HIR_ASSERT(om_omi_parse(&metadata_source, &metadata_module) == OM_STATUS_OK);
    HIR_ASSERT(metadata_module.diagnostic_count == 0);
    const Om_Resolver_Config resolver_config = {
        .native_modules = &metadata_module,
        .native_module_count = 1,
    };
    HIR_ASSERT(test_fixture_has_deterministic_hir(
        "native_decl/hello.bas", 1, "native_decl_hello.hir",
        &resolver_config));
    passed = true;

cleanup:
    om_omi_module_destroy(&metadata_module);
    om_source_text_destroy(&metadata_source);
    om_free(metadata_bytes);
    return passed;
}

static bool test_typed_hir_lower_dump_and_verify(void)
{
    static const char root_path[] = OM_HIR_TEST_FIXTURE_ROOT;
    static const char entry_path[] = "type_inference/positive.bas";
    Om_Source_Root root = {0};
    Om_Module_Project source_project = {0};
    Om_Lexer_Result *lexed = NULL;
    Om_Parser_Result *parsed = NULL;
    Om_Resolution_Result resolution = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result semantic = {0};
    Om_Hir_Project hir = {0};
    Om_Hir_Specialization specialization = {0};
    Om_Hir_Verification_Result verification = {0};
    Om_Hir_Verification_Result invalid_verification = {0};
    Om_Hir_Text first_dump = {0};
    Om_Hir_Text second_dump = {0};
    bool passed = false;

    HIR_ASSERT(test_fixture_has_deterministic_hir(
        "type_inference/positive.bas", 1, "type_inference_positive.hir",
        NULL));
    HIR_ASSERT(test_fixture_has_deterministic_hir(
        "scopes/patterns.bas", 1, "scopes_patterns.hir", NULL));
    HIR_ASSERT(test_fixture_has_deterministic_hir(
        "scopes/siblings.bas", 1, "scopes_siblings.hir", NULL));
    HIR_ASSERT(test_fixture_has_deterministic_hir(
        "visibility/main.bas", 2, "visibility_main.hir", NULL));
    HIR_ASSERT(test_fixture_has_deterministic_hir(
        "system_actor/hello.bas", 1, "system_actor_hello.hir", NULL));
    HIR_ASSERT(test_native_declaration_hir());
    HIR_ASSERT(om_source_root_init(&root,
        (Om_String_View){root_path, sizeof(root_path) - 1}) == OM_STATUS_OK);
    HIR_ASSERT(om_module_project_load(&root,
        (Om_String_View){entry_path, sizeof(entry_path) - 1}, 65536,
        &source_project) == OM_STATUS_OK);
    const size_t module_count = om_module_project_module_count(&source_project);
    HIR_ASSERT(module_count == 1);
    lexed = om_alloc_array(module_count, sizeof(*lexed));
    parsed = om_alloc_array(module_count, sizeof(*parsed));
    HIR_ASSERT(lexed != NULL && parsed != NULL);
    (void)memset(lexed, 0, module_count * sizeof(*lexed));
    (void)memset(parsed, 0, module_count * sizeof(*parsed));
    const Om_Source_Text *source = om_module_project_source(
        &source_project, (Om_Module_Id){0});
    HIR_ASSERT(source != NULL);
    HIR_ASSERT(om_lexer_scan(source, &lexed[0]) == OM_STATUS_OK);
    HIR_ASSERT(lexed[0].diagnostic_count == 0);
    HIR_ASSERT(om_parser_parse(&lexed[0], &parsed[0]) == OM_STATUS_OK);
    HIR_ASSERT(parsed[0].diagnostic_count == 0);
    HIR_ASSERT(om_name_resolve_project(&source_project, parsed, module_count,
        NULL, &resolution) == OM_STATUS_OK);
    HIR_ASSERT(resolution.diagnostic_count == 0);
    HIR_ASSERT(om_type_infer_project(&source_project, parsed, module_count,
        &resolution, &inference) == OM_STATUS_OK);
    HIR_ASSERT(inference.diagnostic_count == 0);
    HIR_ASSERT(om_semantic_analyze_project(&source_project, parsed, module_count,
        &resolution, &inference, &semantic) == OM_STATUS_OK);
    HIR_ASSERT(semantic.diagnostic_count == 0);
    HIR_ASSERT(om_hir_lower_project(&source_project, parsed, module_count,
        &resolution, &inference, &semantic, &hir) == OM_STATUS_OK);
    HIR_ASSERT(hir.module_count == 1 && hir.node_count > 20
               && hir.entity_count >= 8);
    HIR_ASSERT(om_hir_verify_project(&hir, &verification) == OM_STATUS_OK);
    HIR_ASSERT(verification.diagnostic_count == 0);
    const Om_Status specialization_status = om_hir_specialize_project(
        &hir, &inference.type_store, &specialization);
    if (specialization_status != OM_STATUS_OK) {
        (void)fprintf(stderr, "specialization returned %s\n",
                      om_status_name(specialization_status));
    }
    HIR_ASSERT(specialization_status == OM_STATUS_OK);
    HIR_ASSERT(specialization.diagnostic_count == 1);
    HIR_ASSERT(specialization.instance_count > 0
               && specialization.instance_count < 64);
    const Om_Hir_Entity *identity_entity = NULL;
    const Om_Hir_Entity *unwrap_entity = NULL;
    const Om_Hir_Entity *cycle_left_entity = NULL;
    for (size_t index = 0; index < hir.entity_count; ++index) {
        const Om_Hir_Entity *entity = &hir.entities[index];
        if (entity->kind == OM_HIR_ENTITY_FUNCTION
            && entity->name.length == sizeof("identity") - 1
            && memcmp(entity->name.data, "identity", sizeof("identity") - 1) == 0) {
            identity_entity = entity;
        }
        if (entity->kind == OM_HIR_ENTITY_FUNCTION
            && entity->name.length == sizeof("unwrap_result") - 1
            && memcmp(entity->name.data, "unwrap_result",
                      sizeof("unwrap_result") - 1) == 0) {
            unwrap_entity = entity;
        }
        if (entity->kind == OM_HIR_ENTITY_FUNCTION
            && entity->name.length == sizeof("cycle_left") - 1
            && memcmp(entity->name.data, "cycle_left",
                      sizeof("cycle_left") - 1) == 0) {
            cycle_left_entity = entity;
        }
    }
    HIR_ASSERT(identity_entity != NULL && unwrap_entity != NULL
               && cycle_left_entity != NULL);
    bool saw_cycle_left_diagnostic = false;
    for (size_t index = 0; index < specialization.diagnostic_count; ++index) {
        const Om_Specialization_Diagnostic *diagnostic =
            &specialization.diagnostics[index];
        HIR_ASSERT(diagnostic->code == OM_SPECIALIZATION_DIAGNOSTIC_E1002);
        if (diagnostic->symbol_id.value == cycle_left_entity->symbol_id.value) {
            saw_cycle_left_diagnostic = true;
        } else {
            HIR_ASSERT(false);
        }
    }
    HIR_ASSERT(saw_cycle_left_diagnostic);
    bool saw_list_descriptor_type = false;
    bool saw_map_descriptor_type = false;
    bool saw_result_descriptor_type = false;
    for (size_t index = 0; index < specialization.descriptor_type_count;
         ++index) {
        Om_Type_View descriptor_view = {0};
        HIR_ASSERT(om_type_store_get(&inference.type_store,
            specialization.descriptor_types[index], &descriptor_view)
            == OM_STATUS_OK);
        if (descriptor_view.kind == OM_TYPE_LIST) {
            saw_list_descriptor_type = true;
        } else if (descriptor_view.kind == OM_TYPE_MAP) {
            saw_map_descriptor_type = true;
        } else if (descriptor_view.kind == OM_TYPE_RESULT) {
            saw_result_descriptor_type = true;
        } else {
            HIR_ASSERT(false);
        }
    }
    HIR_ASSERT(saw_list_descriptor_type && saw_map_descriptor_type
               && saw_result_descriptor_type);
    size_t identity_instance_count = 0;
    size_t identity_int_count = 0;
    size_t identity_string_count = 0;
    size_t unwrap_instance_count = 0;
    for (size_t index = 0; index < specialization.instance_count; ++index) {
        const Om_Function_Instance *instance = &specialization.instances[index];
        if (instance->symbol_id.value == unwrap_entity->symbol_id.value) {
            HIR_ASSERT(instance->type_argument_count == 2);
            ++unwrap_instance_count;
        }
        if (instance->symbol_id.value != identity_entity->symbol_id.value) {
            continue;
        }
        HIR_ASSERT(instance->type_argument_count == 1);
        const Om_Type_Id argument = specialization.type_arguments[
            instance->type_argument_start];
        Om_Type_Id resolved_argument = OM_TYPE_ID_INVALID;
        Om_Type_View argument_view = {0};
        HIR_ASSERT(om_type_store_resolve(&inference.type_store, argument,
            &resolved_argument) == OM_STATUS_OK);
        HIR_ASSERT(om_type_store_get(&inference.type_store, resolved_argument,
            &argument_view) == OM_STATUS_OK);
        if (argument_view.kind == OM_TYPE_INT) ++identity_int_count;
        if (argument_view.kind == OM_TYPE_STRING) ++identity_string_count;
        const Om_Hir_Node *identity_node = om_hir_get_node(&hir,
                                                           instance->root_node);
        Om_Type_Id specialized_signature = OM_TYPE_ID_INVALID;
        Om_Type_View signature_view = {0};
        Om_Type_Id specialized_parameter = OM_TYPE_ID_INVALID;
        HIR_ASSERT(identity_node != NULL);
        HIR_ASSERT(om_hir_specialization_resolve_type(&hir, &specialization,
            instance->id, identity_node->type_id, &specialized_signature)
            == OM_STATUS_OK);
        HIR_ASSERT(om_type_store_get(&inference.type_store,
            specialized_signature, &signature_view) == OM_STATUS_OK);
        HIR_ASSERT(signature_view.kind == OM_TYPE_FUNCTION
                   && signature_view.child_count == 2);
        HIR_ASSERT(om_type_store_child_at(&inference.type_store,
            specialized_signature, 0, &specialized_parameter) == OM_STATUS_OK);
        Om_Type_Id resolved_parameter = OM_TYPE_ID_INVALID;
        HIR_ASSERT(om_type_store_resolve(&inference.type_store,
            specialized_parameter, &resolved_parameter) == OM_STATUS_OK);
        HIR_ASSERT(resolved_parameter.value == resolved_argument.value);
        ++identity_instance_count;
    }
    HIR_ASSERT(identity_instance_count == 2 && identity_int_count == 1
               && identity_string_count == 1
               && unwrap_instance_count == 1);
    HIR_ASSERT(om_hir_dump(&hir, &first_dump) == OM_STATUS_OK);
    HIR_ASSERT(om_hir_dump(&hir, &second_dump) == OM_STATUS_OK);
    HIR_ASSERT(first_dump.length == second_dump.length
               && memcmp(first_dump.data, second_dump.data,
                         first_dump.length) == 0);
    HIR_ASSERT(contains_text((Om_String_View){first_dump.data, first_dump.length},
                             "kind=try"));
    HIR_ASSERT(contains_text((Om_String_View){first_dump.data, first_dump.length},
                             "kind=actor"));

    Om_Hir_Node *mutable_nodes = (Om_Hir_Node *)hir.nodes;
    Om_Hir_Node *try_node = NULL;
    Om_Hir_Node *send_node = NULL;
    Om_Hir_Node *function_node = NULL;
    Om_Hir_Node *ordinary_binding = NULL;
    for (size_t index = 0; index < hir.node_count; ++index) {
        Om_Hir_Node *node = &mutable_nodes[index];
        if (node->kind == OM_HIR_TRY) try_node = node;
        if (node->kind == OM_HIR_SEND) send_node = node;
        if (node->kind == OM_HIR_FUNCTION) function_node = node;
        if (ordinary_binding == NULL
            && (node->kind == OM_HIR_BIND || node->kind == OM_HIR_DECLARE_VAR)) {
            ordinary_binding = node;
        }
    }
    HIR_ASSERT(try_node != NULL && try_node->child_count == 1);
    Om_Hir_Node *try_input = &mutable_nodes[
        hir.children[try_node->child_start].value];
    const Om_Type_Id saved_input_type = try_input->type_id;
    Om_Type_Id integer_type = OM_TYPE_ID_INVALID;
    HIR_ASSERT(om_type_store_primitive(hir.type_store, OM_TYPE_INT,
                                       &integer_type) == OM_STATUS_OK);
    try_input->type_id = integer_type;
    HIR_ASSERT(om_hir_verify_project(&hir, &invalid_verification) == OM_STATUS_OK);
    HIR_ASSERT(has_hir_diagnostic(&invalid_verification,
                                  OM_HIR_DIAGNOSTIC_E0906));
    try_input->type_id = saved_input_type;
    om_hir_verification_result_destroy(&invalid_verification);

    HIR_ASSERT(function_node != NULL);
    const uint32_t saved_function_effects = function_node->effects;
    function_node->effects |= (uint32_t)OM_EFFECT_SEND_MESSAGE;
    HIR_ASSERT(om_hir_verify_project(&hir, &invalid_verification) == OM_STATUS_OK);
    HIR_ASSERT(has_hir_diagnostic(&invalid_verification,
                                  OM_HIR_DIAGNOSTIC_E0907));
    function_node->effects = saved_function_effects;
    om_hir_verification_result_destroy(&invalid_verification);

    HIR_ASSERT(send_node != NULL);
    const Om_Symbol_Id saved_send_protocol = send_node->related_symbol_id;
    send_node->related_symbol_id = OM_SYMBOL_ID_INVALID;
    HIR_ASSERT(om_hir_verify_project(&hir, &invalid_verification) == OM_STATUS_OK);
    HIR_ASSERT(has_hir_diagnostic(&invalid_verification,
                                  OM_HIR_DIAGNOSTIC_E0905));
    send_node->related_symbol_id = saved_send_protocol;
    om_hir_verification_result_destroy(&invalid_verification);

    HIR_ASSERT(ordinary_binding != NULL);
    const Om_Hir_Kind saved_binding_kind = ordinary_binding->kind;
    ordinary_binding->kind = OM_HIR_ACTOR_STATE_ASSIGN;
    HIR_ASSERT(om_hir_verify_project(&hir, &invalid_verification) == OM_STATUS_OK);
    HIR_ASSERT(has_hir_diagnostic(&invalid_verification,
                                  OM_HIR_DIAGNOSTIC_E0908));
    ordinary_binding->kind = saved_binding_kind;
    om_hir_verification_result_destroy(&invalid_verification);

    const Om_Type_Id saved_type = mutable_nodes[0].type_id;
    mutable_nodes[0].kind = OM_HIR_LOCAL;
    mutable_nodes[0].type_id = OM_TYPE_ID_INVALID;
    mutable_nodes[0].symbol_id = OM_SYMBOL_ID_INVALID;
    HIR_ASSERT(om_hir_verify_project(&hir, &invalid_verification) == OM_STATUS_OK);
    HIR_ASSERT(invalid_verification.diagnostic_count >= 2);
    mutable_nodes[0].kind = OM_HIR_PROGRAM;
    mutable_nodes[0].type_id = saved_type;
    mutable_nodes[0].symbol_id = OM_SYMBOL_ID_INVALID;
    passed = true;

cleanup:
    om_hir_specialization_destroy(&specialization);
    om_hir_text_destroy(&second_dump);
    om_hir_text_destroy(&first_dump);
    om_hir_verification_result_destroy(&invalid_verification);
    om_hir_verification_result_destroy(&verification);
    om_hir_project_destroy(&hir);
    om_semantic_analysis_result_destroy(&semantic);
    om_type_inference_result_destroy(&inference);
    om_resolution_result_destroy(&resolution);
    if (parsed != NULL) {
        const size_t count = om_module_project_module_count(&source_project);
        for (size_t index = 0; index < count; ++index) {
            om_parser_result_destroy(&parsed[index]);
        }
    }
    if (lexed != NULL) {
        const size_t count = om_module_project_module_count(&source_project);
        for (size_t index = 0; index < count; ++index) {
            om_lexer_result_destroy(&lexed[index]);
        }
    }
    om_free(parsed);
    om_free(lexed);
    om_module_project_destroy(&source_project);
    om_source_root_destroy(&root);
    return passed;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--update-goldens") == 0) {
        update_hir_goldens = true;
    } else if (argc != 1) {
        (void)fprintf(stderr, "Usage: ombasic_hir_tests [--update-goldens]\n");
        return 2;
    }
    const bool passed = test_typed_hir_lower_dump_and_verify();
    (void)fprintf(stdout, "HIR lowering tests: %s\n", passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
