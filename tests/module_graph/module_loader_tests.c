#include "test.h"

#include <string.h>

#include "ombasic/module_graph/module_loader.h"

#if !defined(OM_MODULE_TEST_FIXTURE_ROOT)
#error OM_MODULE_TEST_FIXTURE_ROOT must name the checked-in loader fixture directory
#endif

static bool initialize_fixture_root(Om_Source_Root *root)
{
    static const char fixture_root[] = OM_MODULE_TEST_FIXTURE_ROOT;
    return om_source_root_init(
               root,
               (Om_String_View){fixture_root, sizeof(fixture_root) - 1}
           ) == OM_STATUS_OK;
}

bool om_test_module_project_load(void)
{
    Om_Source_Root root = {0};
    Om_Module_Project project = {0};
    bool passed = false;

    if (!initialize_fixture_root(&root)) {
        goto cleanup;
    }
    const Om_Status load_status = om_module_project_load(
        &root,
        (Om_String_View){"acyclic/main.bas", 16},
        1024,
        &project
    );
    if (load_status != OM_STATUS_OK) {
        const Om_String_View failed_path = om_module_graph_path(
            &project.graph,
            project.failed_module
        );
        (void)fprintf(stderr,
                      "module project load returned %s for %.*s at %u..%u\n",
                      om_status_name(load_status),
                      (int)failed_path.length,
                      failed_path.data == NULL ? "" : failed_path.data,
                      project.error_span.start,
                      project.error_span.end);
        goto cleanup;
    }
    if (project.order.length != 3 || project.order.cycle_length != 0) {
        goto cleanup;
    }
    const Om_String_View main_path = om_module_graph_path(&project.graph, (Om_Module_Id){0});
    const Om_String_View math_path = om_module_graph_path(&project.graph, (Om_Module_Id){1});
    const Om_String_View helper_path = om_module_graph_path(&project.graph, (Om_Module_Id){2});
    const Om_Source_Text *main_source = om_module_project_source(&project, (Om_Module_Id){0});
    const Om_Source_Text *helper_source = om_module_project_source(&project, (Om_Module_Id){2});
    Om_Module_Import_Info first_import = {0};
    Om_Module_Import_Info second_import = {0};
    const Om_Status first_import_status = om_module_project_import_at(
        &project, (Om_Module_Id){0}, 0, &first_import);
    const Om_Status second_import_status = om_module_project_import_at(
        &project, (Om_Module_Id){0}, 1, &second_import);
    if (!om_string_view_equal(main_path, (Om_String_View){"acyclic/main.bas", 16})
        || !om_string_view_equal(math_path, (Om_String_View){"acyclic/math.bas", 16})
        || !om_string_view_equal(helper_path, (Om_String_View){"acyclic/nested/helper.bas", 25})
        || main_source == NULL || helper_source == NULL
        || project.order.items[0].value != 2 || project.order.items[1].value != 1
        || project.order.items[2].value != 0
        || om_module_project_import_count(&project, (Om_Module_Id){0}) != 2
        || first_import_status != OM_STATUS_OK
        || first_import.target_module.value != 1
        || !om_string_view_equal(first_import.path,
                                 (Om_String_View){"acyclic/math.bas", 16})
        || first_import.span.source.value != 0
        || second_import_status != OM_STATUS_OK
        || second_import.target_module.value != 2
        || !om_string_view_equal(second_import.path,
                                 (Om_String_View){"acyclic/nested/helper.bas", 25})
        || project.error_span.source.value != OM_SOURCE_ID_INVALID.value) {
        (void)fprintf(stderr,
                      "module project shape: order=%zu cycle=%zu ids=%u,%u,%u err_source=%u\n",
                      project.order.length,
                      project.order.cycle_length,
                      project.order.length > 0 ? project.order.items[0].value : UINT32_MAX,
                      project.order.length > 1 ? project.order.items[1].value : UINT32_MAX,
                      project.order.length > 2 ? project.order.items[2].value : UINT32_MAX,
                      project.error_span.source.value);
        goto cleanup;
    }
    Om_Module_Import_Info out_of_range_import = {0};
    if (om_module_project_import_at(&project, (Om_Module_Id){0}, 2,
                                    &out_of_range_import)
            != OM_STATUS_OUT_OF_RANGE
        || out_of_range_import.target_module.value != UINT32_MAX) {
        goto cleanup;
    }
    if (om_module_project_source(&project, OM_MODULE_ID_INVALID) != NULL) {
        goto cleanup;
    }
    om_module_project_destroy(&project);
    const Om_Status escaped_status = om_module_project_load(
        &root,
        (Om_String_View){"acyclic/escaped.bas", 19},
        1024,
        &project
    );
    if (escaped_status != OM_STATUS_OK) {
        (void)fprintf(stderr, "escaped import load returned %s\n", om_status_name(escaped_status));
        goto cleanup;
    }
    if (project.order.length != 3
        || project.order.items[0].value != 2
        || project.order.items[1].value != 1
        || project.order.items[2].value != 0
        || !om_string_view_equal(
            om_module_graph_path(&project.graph, (Om_Module_Id){1}),
            (Om_String_View){"acyclic/math.bas", 16}
        )) {
        goto cleanup;
    }
    passed = true;

cleanup:
    om_module_project_destroy(&project);
    om_source_root_destroy(&root);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_module_project_cycle(void)
{
    Om_Source_Root root = {0};
    Om_Module_Project project = {0};
    Om_Module_Graph_Cycle_Message message = {0};
    bool passed = false;
    static const char expected[] = "cycle_a.bas -> cycle_b.bas -> cycle_c.bas -> cycle_a.bas";

    if (!initialize_fixture_root(&root)) {
        goto cleanup;
    }
    if (om_module_project_load(
            &root,
            (Om_String_View){"cycle_a.bas", 11},
            1024,
            &project
        ) != OM_STATUS_CYCLE_DETECTED) {
        goto cleanup;
    }
    if (om_module_graph_cycle_message(&project.graph, &project.order, &message) != OM_STATUS_OK
        || message.length != sizeof(expected) - 1
        || memcmp(message.data, expected, sizeof(expected)) != 0) {
        goto cleanup;
    }
    passed = true;

cleanup:
    om_module_graph_cycle_message_destroy(&message);
    om_module_project_destroy(&project);
    om_source_root_destroy(&root);
    OM_TEST_ASSERT(passed);
    return true;
}

static bool expect_project_error(
    Om_Source_Root *root,
    const char *entry_path,
    size_t entry_path_length,
    Om_Status expected_status,
    const char *expected_span_text,
    size_t expected_span_length
)
{
    Om_Module_Project project = {0};
    bool passed = false;
    const Om_Status status = om_module_project_load(
        root,
        (Om_String_View){entry_path, entry_path_length},
        1024,
        &project
    );
    if (status != expected_status || project.failed_module.value != 0
        || project.error_span.source.value != 0) {
        goto cleanup;
    }
    const Om_Source_Text *source = om_module_project_source(&project, (Om_Module_Id){0});
    if (source == NULL) {
        goto cleanup;
    }
    Om_Const_Byte_Slice selected = {0};
    if (om_source_text_slice(source, project.error_span, &selected) != OM_STATUS_OK
        || selected.length != expected_span_length
        || memcmp(selected.data, expected_span_text, expected_span_length) != 0) {
        goto cleanup;
    }
    passed = true;

cleanup:
    om_module_project_destroy(&project);
    return passed;
}

bool om_test_module_project_errors(void)
{
    Om_Source_Root root = {0};
    bool passed = false;
    if (!initialize_fixture_root(&root)) {
        goto cleanup;
    }
    if (!expect_project_error(
            &root,
            "missing_ref.bas",
            sizeof("missing_ref.bas") - 1,
            OM_STATUS_NOT_FOUND,
            "\"missing.bas\"",
            13
        )
        || !expect_project_error(
            &root,
            "invalid_ref.bas",
            sizeof("invalid_ref.bas") - 1,
            OM_STATUS_INVALID_ARGUMENT,
            "\"../outside.bas\"",
            16
        )
        || !expect_project_error(
            &root,
            "malformed_ref.bas",
            sizeof("malformed_ref.bas") - 1,
            OM_STATUS_INVALID_ARGUMENT,
            "\"not_closed.bas\n",
            sizeof("\"not_closed.bas\n") - 1
        )) {
        goto cleanup;
    }
    passed = true;

cleanup:
    om_source_root_destroy(&root);
    OM_TEST_ASSERT(passed);
    return true;
}
