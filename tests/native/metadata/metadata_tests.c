#include "ombasic/native/metadata/omi.h"
#include "ombasic/native/metadata/omi_project.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/source/source_text.h"

#if !defined(OM_METADATA_TEST_FIXTURE_ROOT)
#error OM_METADATA_TEST_FIXTURE_ROOT must name the metadata fixture directory
#endif

static bool parse_text(const char *text, Om_Source_Text *source,
                       Om_Omi_Module *module)
{
    static const char path[] = "native_test.omi";
    const Om_Status status = om_source_text_init(
        source, (Om_Source_Id){91},
        (Om_String_View){path, sizeof(path) - 1},
        (Om_Const_Byte_Slice){(const uint8_t *)text, strlen(text)}, 0);
    return status == OM_STATUS_OK && om_omi_parse(source, module) == OM_STATUS_OK;
}

static bool has_diagnostic(const Om_Omi_Module *module,
                           Om_Omi_Diagnostic_Code expected)
{
    for (size_t index = 0; index < module->diagnostic_count; ++index) {
        if (module->diagnostics[index].code == expected) {
            return true;
        }
    }
    return false;
}

static bool test_native_metadata(void)
{
    static const char input[] =
        "NATIVE \"native_\\u{e9}\"\n"
        "FUNCTION add(left: CInt, right: CInt): CInt\n"
        "    SYMBOL \"native_add\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION create_name(): CString\n"
        "    SYMBOL \"native_create_name\"\n"
        "    EFFECT NATIVE\n"
        "    BLOCKING\n"
        "    RETURNS OWNED WITH release_name\n"
        "END FUNCTION\n"
        "FUNCTION release_name(value: CPointer OWNED): CVoid\n"
        "    SYMBOL \"native_release_name\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "NATIVE RECORD C_Point\n"
        "    x: CDouble\n"
        "    label: CString STATIC\n"
        "END NATIVE RECORD\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    static const char expected_library[] = "native_\xc3\xa9";
    if (!parse_text(input, &source, &module)) {
        goto cleanup;
    }
    if (module.diagnostic_count != 0 || module.function_count != 3
        || module.record_count != 1 || module.record_field_count != 2
        || module.functions[0].blocking != OM_NATIVE_BLOCKING_NO
        || module.functions[1].result_ownership != OM_NATIVE_OWNERSHIP_OWNED
        || module.record_fields[1].ownership != OM_NATIVE_OWNERSHIP_STATIC
        || module.library_path.length != sizeof(expected_library) - 1
        || memcmp(module.library_path.data, expected_library,
                  sizeof(expected_library) - 1) != 0) {
        (void)fprintf(stderr,
            "metadata counts: diagnostics=%zu functions=%zu records=%zu fields=%zu library=%.*s\n",
            module.diagnostic_count, module.function_count, module.record_count,
            module.record_field_count, (int)module.library_path.length,
            module.library_path.data);
        goto cleanup;
    }
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_callback_metadata(void)
{
    static const char input[] =
        "NATIVE \"callback_fixture\"\n"
        "FUNCTION subscribe(callback: CALLBACK(CInt, CDouble) -> CVoid): CVoid\n"
        "    SYMBOL \"callback_subscribe\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "    CALLBACK callback FOREIGN_THREAD MESSAGE Tick\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    if (!parse_text(input, &source, &module)
        || module.diagnostic_count != 0 || module.function_count != 1
        || module.parameter_count != 1
        || module.callback_parameter_count != 2
        || module.callback_binding_count != 1
        || module.functions == NULL || module.parameters == NULL
        || module.callback_parameters == NULL
        || module.callback_bindings == NULL) {
        goto cleanup;
    }
    const Om_Omi_Parameter *callback = &module.parameters[0];
    const Om_Omi_Native_Function *function = &module.functions[0];
    const Om_Omi_Callback_Binding *binding = &module.callback_bindings[0];
    passed = callback->is_callback
        && callback->callback_result_type == OM_NATIVE_ABI_CVOID
        && callback->callback_parameter_start == 0
        && callback->callback_parameter_count == 2
        && module.callback_parameters[0].abi_type == OM_NATIVE_ABI_CINT
        && module.callback_parameters[1].abi_type == OM_NATIVE_ABI_CDOUBLE
        && function->callback_binding_start == 0
        && function->callback_binding_count == 1
        && binding->parameter_index == 0
        && binding->message_name.length == 4
        && binding->message_name.data != NULL
        && memcmp(binding->message_name.data, "Tick", 4) == 0;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_callback_requires_foreign_thread_message_binding(void)
{
    static const char input[] =
        "NATIVE \"callback_fixture\"\n"
        "FUNCTION subscribe(callback: CALLBACK(CInt) -> CVoid): CVoid\n"
        "    SYMBOL \"callback_subscribe\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    if (!parse_text(input, &source, &module)
        || !has_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0806)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_callback_rejects_nonvoid_result(void)
{
    static const char input[] =
        "NATIVE \"callback_fixture\"\n"
        "FUNCTION subscribe(callback: CALLBACK(CInt) -> CInt): CVoid\n"
        "    SYMBOL \"callback_subscribe\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "    CALLBACK callback FOREIGN_THREAD MESSAGE Tick\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    if (!parse_text(input, &source, &module)
        || !has_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0804)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_invalid_ownership_diagnostic(void)
{
    static const char input[] =
        "NATIVE \"native_invalid\"\n"
        "FUNCTION bad(value: CInt BORROWED): CString\n"
        "    SYMBOL \"native_bad\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    if (!parse_text(input, &source, &module)
        || !has_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0805)
        || strcmp(om_omi_diagnostic_code_name(OM_OMI_DIAGNOSTIC_E0805),
                  "E0805") != 0) {
        goto cleanup;
    }
    for (size_t index = 0; index < module.diagnostic_count; ++index) {
        if (!om_source_span_is_valid(module.diagnostics[index].span,
                                     (uint32_t)source.bytes.length)) {
            goto cleanup;
        }
    }
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_borrowed_native_pointer_requires_native_owner(void)
{
    static const char input[] =
        "NATIVE \"native_pointer_owner\"\n"
        "FUNCTION invalid_owner(value: CString BORROWED): CPointer\n"
        "    SYMBOL \"native_invalid_owner\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "    RETURNS BORROWED FROM value\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    if (!parse_text(input, &source, &module)
        || !has_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0805)) {
        goto cleanup;
    }
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_malformed_input_fuzz(void)
{
    static const char alphabet[] =
        "NATIVE FUNCTION RECORD END EFFECT SYMBOL BLOCKING NONBLOCKING "
        "RETURNS CALLBACK BORROWED OWNED STATIC CInt CVoid CPointer CString "
        "NATIVE_RECORD(),:->'\"\\ abcxyz\n\r\t";
    uint32_t random = UINT32_C(0x8f31a2c7);
    for (uint32_t round = 0; round < UINT32_C(512); ++round) {
        char bytes[96] = {0};
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        const size_t length = random % (uint32_t)sizeof(bytes);
        for (size_t index = 0; index < length; ++index) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[index] = alphabet[(random >> 16)
                % (uint32_t)(sizeof(alphabet) - 1)];
        }
        Om_Source_Text source = {0};
        Om_Omi_Module module = {0};
        static const char path[] = "fuzz.omi";
        const Om_Status source_status = om_source_text_init(
            &source, (Om_Source_Id){1},
            (Om_String_View){path, sizeof(path) - 1},
            (Om_Const_Byte_Slice){(const uint8_t *)bytes, length}, 0);
        if (source_status == OM_STATUS_OK) {
            if (om_omi_parse(&source, &module) != OM_STATUS_OK) {
                om_omi_module_destroy(&module);
                om_source_text_destroy(&source);
                return false;
            }
        } else {
            om_omi_module_destroy(&module);
            om_source_text_destroy(&source);
            return false;
        }
        bool valid = true;
        for (size_t index = 0; index < module.diagnostic_count; ++index) {
            valid = valid && om_source_span_is_valid(
                module.diagnostics[index].span, (uint32_t)length);
        }
        for (size_t index = 0; index < module.function_count; ++index) {
            const Om_Omi_Native_Function *function = &module.functions[index];
            valid = valid && function->parameter_start <= module.parameter_count
                && function->parameter_count
                    <= module.parameter_count - function->parameter_start
                && function->callback_binding_start
                    <= module.callback_binding_count
                && function->callback_binding_count
                    <= module.callback_binding_count
                        - function->callback_binding_start;
        }
        for (size_t index = 0; index < module.record_count; ++index) {
            const Om_Omi_Native_Record *record = &module.records[index];
            valid = valid && record->field_start <= module.record_field_count
                && record->field_count
                    <= module.record_field_count - record->field_start;
        }
        om_omi_module_destroy(&module);
        om_source_text_destroy(&source);
        if (!valid) {
            return false;
        }
    }
    return true;
}

static bool test_metadata_project_loader(void)
{
    static const char root_path[] = OM_METADATA_TEST_FIXTURE_ROOT;
    static const char metadata_path[] = "math.omi";
    Om_Source_Root root = {0};
    Om_Omi_Project project = {0};
    const Om_String_View paths[] = {{metadata_path, sizeof(metadata_path) - 1}};
    bool passed = false;
    if (om_source_root_init(&root,
            (Om_String_View){root_path, sizeof(root_path) - 1}) != OM_STATUS_OK
        || om_omi_project_load(&root, paths, 1, UINT32_C(1000), 4096,
                               &project) != OM_STATUS_OK) {
        goto cleanup;
    }
    const Om_Omi_Module *module = om_omi_project_module(&project, 0);
    const Om_Source_Text *source = om_omi_project_source(&project, 0);
    passed = module != NULL && source != NULL
        && module->diagnostic_count == 0 && module->function_count == 1;

cleanup:
    om_omi_project_destroy(&project);
    om_source_root_destroy(&root);
    return passed;
}

int main(void)
{
    const bool results[] = {
        test_native_metadata(),
        test_callback_metadata(),
        test_callback_requires_foreign_thread_message_binding(),
        test_callback_rejects_nonvoid_result(),
        test_invalid_ownership_diagnostic(),
        test_borrowed_native_pointer_requires_native_owner(),
        test_malformed_input_fuzz(),
        test_metadata_project_loader(),
    };
    static const char *const names[] = {
        "native metadata", "callback metadata",
        "callback binding requirement", "callback result ABI",
        "invalid ownership",
        "borrowed pointer owner validation", "malformed input fuzz",
        "metadata project loader",
    };
    size_t passed = 0;
    const size_t total = sizeof(results) / sizeof(results[0]);
    for (size_t index = 0; index < total; ++index) {
        passed += results[index] ? 1u : 0u;
        (void)fprintf(stdout, "%s: %s\n", results[index] ? "PASS" : "FAIL",
                      names[index]);
    }
    (void)fprintf(stdout, "Native metadata tests: %zu/%zu passed\n", passed, total);
    return passed == total ? 0 : 1;
}
