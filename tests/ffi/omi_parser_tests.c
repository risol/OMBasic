#include "ombasic/ffi/omi.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/ffi/abi_type.h"
#include "ombasic/ffi/c_string.h"
#include "ombasic/ffi/dynamic_library.h"
#include "ombasic/ffi/native_call.h"
#include "ombasic/ffi/omi_project.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#if !defined(OM_OMI_PROJECT_TEST_ROOT)
#error OM_OMI_PROJECT_TEST_ROOT must name the Native metadata fixture root
#endif

#define OMI_ASSERT(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",            \
                          __FILE__, __LINE__, #expression);                      \
            goto cleanup;                                                        \
        }                                                                        \
    } while (false)

static bool parse_text(const char *text, Om_Source_Text *source,
                       Om_Omi_Module *module)
{
    static const char path[] = "native_test.omi";
    const size_t text_length = strlen(text);
    const Om_Status source_status = om_source_text_init(
        source, (Om_Source_Id){91},
        (Om_String_View){path, sizeof(path) - 1},
        (Om_Const_Byte_Slice){(const uint8_t *)text, text_length}, 0);
    return source_status == OM_STATUS_OK
        && om_omi_parse(source, module) == OM_STATUS_OK;
}

static bool contains_diagnostic(const Om_Omi_Module *module,
                                Om_Omi_Diagnostic_Code code)
{
    for (size_t index = 0; index < module->diagnostic_count; ++index) {
        if (module->diagnostics[index].code == code) {
            return true;
        }
    }
    return false;
}

static bool test_complete_native_metadata(void)
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
        "FUNCTION register_tick(callback: CALLBACK(CInt, CPointer) -> CVoid): CVoid\n"
        "    SYMBOL \"native_register_tick\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "    CALLBACK callback FOREIGN_THREAD MESSAGE Tick\n"
        "END FUNCTION\n"
        "NATIVE RECORD C_Point\n"
        "    x: CDouble\n"
        "    label: CString STATIC\n"
        "END NATIVE RECORD\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    OMI_ASSERT(parse_text(input, &source, &module));
    OMI_ASSERT(module.diagnostic_count == 0);
    OMI_ASSERT(module.function_count == 4);
    OMI_ASSERT(module.parameter_count == 4);
    OMI_ASSERT(module.callback_parameter_count == 2);
    OMI_ASSERT(module.callback_binding_count == 1);
    OMI_ASSERT(module.record_count == 1);
    OMI_ASSERT(module.record_field_count == 2);
    static const char expected_path[] = "native_\xc3\xa9";
    OMI_ASSERT(module.library_path.length == sizeof(expected_path) - 1);
    OMI_ASSERT(memcmp(module.library_path.data, expected_path,
                      module.library_path.length) == 0);
    OMI_ASSERT(module.functions[0].result_type == OM_NATIVE_ABI_CINT);
    OMI_ASSERT(module.functions[0].effects == OM_NATIVE_EFFECT_NONE);
    OMI_ASSERT(module.functions[0].blocking == OM_NATIVE_BLOCKING_NO);
    OMI_ASSERT(module.functions[1].result_type == OM_NATIVE_ABI_CSTRING);
    OMI_ASSERT(module.functions[1].result_ownership
               == OM_NATIVE_OWNERSHIP_OWNED);
    OMI_ASSERT(module.functions[1].release_function.length
               == strlen("release_name"));
    OMI_ASSERT(module.functions[3].callback_binding_count == 1);
    OMI_ASSERT(module.callback_bindings[0].parameter_index == 3);
    OMI_ASSERT(module.callback_bindings[0].message_name.length
               == strlen("Tick"));
    OMI_ASSERT(module.records[0].field_count == 2);
    OMI_ASSERT(module.record_fields[1].abi_type == OM_NATIVE_ABI_CSTRING);
    OMI_ASSERT(module.record_fields[1].ownership == OM_NATIVE_OWNERSHIP_STATIC);
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_invalid_ownership_metadata(void)
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
    OMI_ASSERT(parse_text(input, &source, &module));
    OMI_ASSERT(module.diagnostic_count >= 2);
    OMI_ASSERT(contains_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0805));
    OMI_ASSERT(strcmp(om_omi_diagnostic_code_name(
                         OM_OMI_DIAGNOSTIC_E0805), "E0805") == 0);
    for (size_t index = 0; index < module.diagnostic_count; ++index) {
        OMI_ASSERT(om_source_span_is_valid(
            module.diagnostics[index].span,
            (uint32_t)source.bytes.length));
    }
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_callback_and_syntax_diagnostics(void)
{
    static const char input[] =
        "NATIVE \"native_bad_callback\"\n"
        "FUNCTION bad_callback(callback: CALLBACK() -> CVoid): CVoid\n"
        "    SYMBOL \"native_bad_callback\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION BadName(): CInt\n"
        "    SYMBOL \"native_bad_name\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION bad_type(): CUnknown\n"
        "    SYMBOL \"native_bad_type\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION missing_record(value: C_Pair): CInt\n"
        "    SYMBOL \"native_missing_record\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    bool passed = false;
    OMI_ASSERT(parse_text(input, &source, &module));
    OMI_ASSERT(module.diagnostic_count >= 3);
    OMI_ASSERT(contains_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0802));
    OMI_ASSERT(contains_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0804));
    OMI_ASSERT(contains_diagnostic(&module, OM_OMI_DIAGNOSTIC_E0806));
    passed = true;

cleanup:
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_c_abi_type_and_record_layout(void)
{
    struct C_Point_Probe {
        int8_t tag;
        double x;
        char *label;
    };
    const Om_Omi_Record_Field fields[] = {
        {.abi_type = OM_NATIVE_ABI_CINT8},
        {.abi_type = OM_NATIVE_ABI_CDOUBLE},
        {.abi_type = OM_NATIVE_ABI_CSTRING}
    };
    size_t offsets[sizeof(fields) / sizeof(fields[0])] = {0};
    Om_Native_Record_Layout layout = {0};
    Om_Native_Abi_Type_Info type_info = {0};
    OMI_ASSERT(om_native_record_layout(
                   fields, sizeof(fields) / sizeof(fields[0]), offsets,
                   sizeof(offsets) / sizeof(offsets[0]), &layout)
               == OM_STATUS_OK);
    OMI_ASSERT(offsets[0] == offsetof(struct C_Point_Probe, tag));
    OMI_ASSERT(offsets[1] == offsetof(struct C_Point_Probe, x));
    OMI_ASSERT(offsets[2] == offsetof(struct C_Point_Probe, label));
    OMI_ASSERT(layout.size == sizeof(struct C_Point_Probe));
    OMI_ASSERT(layout.alignment == _Alignof(struct C_Point_Probe));
    OMI_ASSERT(om_native_abi_type_info(OM_NATIVE_ABI_CSIZE, &type_info)
               == OM_STATUS_OK);
    OMI_ASSERT(type_info.size == sizeof(size_t));
    OMI_ASSERT(type_info.alignment == _Alignof(size_t));
    OMI_ASSERT(om_native_abi_type_info(OM_NATIVE_ABI_CLONG, &type_info)
               == OM_STATUS_OK);
    OMI_ASSERT(type_info.size == sizeof(long));
    OMI_ASSERT(om_native_abi_type_info(OM_NATIVE_ABI_INVALID, &type_info)
               == OM_STATUS_INVALID_ARGUMENT);
    return true;

cleanup:
    return false;
}

static bool test_dynamic_c_roundtrip(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    static const char input[] =
        "NATIVE \"native_fixture\"\n"
        "FUNCTION add(left: CInt, right: CInt): CInt\n"
        "    SYMBOL \"om_ffi_add\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION mixed(left: CInt, scale: CDouble, right: CInt): CDouble\n"
        "    SYMBOL \"om_ffi_mixed\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION utf8_length(value: CString BORROWED): CSize\n"
        "    SYMBOL \"om_ffi_utf8_length\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION echo_i8(value: CInt8): CInt8\n"
        "    SYMBOL \"om_ffi_echo_i8\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "FUNCTION echo_string(value: CString BORROWED): CString\n"
        "    SYMBOL \"om_ffi_echo_string\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "    RETURNS BORROWED FROM value\n"
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
        "FUNCTION echo_point(value: C_Point): C_Point\n"
        "    SYMBOL \"om_ffi_echo_point\"\n"
        "    EFFECT PURE\n"
        "    NONBLOCKING\n"
        "END FUNCTION\n"
        "NATIVE RECORD C_Point\n"
        "    x: CInt32\n"
        "    y: CDouble\n"
        "END NATIVE RECORD\n"
        "END NATIVE\n";
    static const char path[] = OM_NATIVE_FIXTURE_PATH;
    static const char utf8_value[] = "name_\xc3\xa9";
    Om_Native_Library library = {0};
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    Om_Native_Call_Binding add_binding = {0};
    Om_Native_Call_Binding mixed_binding = {0};
    Om_Native_Call_Binding utf8_binding = {0};
    Om_Native_Call_Binding bounded_utf8_binding = {0};
    Om_Native_Call_Binding i8_binding = {0};
    Om_Native_Call_Binding echo_string_binding = {0};
    Om_Native_Call_Binding owned_binding = {0};
    Om_Native_Call_Binding echo_point_binding = {0};
    Om_Native_Call_Result call_result = {0};
    Om_Native_Value add_arguments[] = {
        {.kind = OM_NATIVE_VALUE_SCALAR,
         .abi_type = OM_NATIVE_ABI_CINT,
         .as.signed_integer = 19},
        {.kind = OM_NATIVE_VALUE_SCALAR,
         .abi_type = OM_NATIVE_ABI_CINT,
         .as.signed_integer = 23}
    };
    Om_Native_Value mixed_arguments[] = {
        {.kind = OM_NATIVE_VALUE_SCALAR,
         .abi_type = OM_NATIVE_ABI_CINT,
         .as.signed_integer = 2},
        {.kind = OM_NATIVE_VALUE_SCALAR,
         .abi_type = OM_NATIVE_ABI_CDOUBLE,
         .as.floating_point = 1.5},
        {.kind = OM_NATIVE_VALUE_SCALAR,
         .abi_type = OM_NATIVE_ABI_CINT,
         .as.signed_integer = 4}
    };
    Om_Native_Value utf8_arguments[] = {
        {.kind = OM_NATIVE_VALUE_UTF8_STRING,
         .abi_type = OM_NATIVE_ABI_CSTRING,
         .as.utf8_string = {utf8_value, sizeof(utf8_value) - 1}}
    };
    Om_Native_Value out_of_range_i8[] = {
        {.kind = OM_NATIVE_VALUE_SCALAR,
         .abi_type = OM_NATIVE_ABI_CINT8,
         .as.signed_integer = 128}
    };
    Om_Native_Value valid_i8[] = {
        {.kind = OM_NATIVE_VALUE_SCALAR,
         .abi_type = OM_NATIVE_ABI_CINT8,
         .as.signed_integer = -128}
    };
    struct Om_Test_Point {
        int32_t x;
        double y;
    } point = {.x = 13, .y = 2.5};
    Om_Native_Value point_arguments[] = {
        {.kind = OM_NATIVE_VALUE_RECORD,
         .abi_type = OM_NATIVE_ABI_RECORD,
         .record_index = 0,
         .as.record = {.data = &point, .size = sizeof(point)}}
    };
    static const char invalid_utf8[] = {(char)0xc3, 'x'};
    bool passed = false;
    OMI_ASSERT(parse_text(input, &source, &module));
    OMI_ASSERT(module.diagnostic_count == 0);
    OMI_ASSERT(module.functions[7].result_type == OM_NATIVE_ABI_RECORD);
    OMI_ASSERT(module.functions[7].result_record_index == 0);
    OMI_ASSERT(module.parameters[9].abi_type == OM_NATIVE_ABI_RECORD);
    OMI_ASSERT(module.parameters[9].record_index == 0);
    void *symbol_address = NULL;
    OMI_ASSERT(om_native_library_open(
                   &library, (Om_String_View){path, sizeof(path) - 1})
               == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &add_binding, &library, &module, 0, 0) == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &mixed_binding, &library, &module, 1, 0) == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &utf8_binding, &library, &module, 2, 0) == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &bounded_utf8_binding, &library, &module, 2,
                   sizeof(utf8_value) - 2) == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &i8_binding, &library, &module, 3, 0) == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &echo_string_binding, &library, &module, 4, 0)
               == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &owned_binding, &library, &module, 5, 0) == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &echo_point_binding, &library, &module, 7, 0)
               == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_invoke(
                   &add_binding, add_arguments,
                   sizeof(add_arguments) / sizeof(add_arguments[0]),
                   &call_result) == OM_STATUS_OK);
    OMI_ASSERT(call_result.value.kind == OM_NATIVE_VALUE_SCALAR);
    OMI_ASSERT(call_result.value.abi_type == OM_NATIVE_ABI_CINT);
    OMI_ASSERT(call_result.value.as.signed_integer == 42);
    OMI_ASSERT(om_native_call_invoke(
                   &mixed_binding, mixed_arguments,
                   sizeof(mixed_arguments) / sizeof(mixed_arguments[0]),
                   &call_result) == OM_STATUS_OK);
    OMI_ASSERT(call_result.value.abi_type == OM_NATIVE_ABI_CDOUBLE);
    OMI_ASSERT(call_result.value.as.floating_point == 8.0);
    OMI_ASSERT(om_native_call_invoke(
                   &utf8_binding, utf8_arguments,
                   sizeof(utf8_arguments) / sizeof(utf8_arguments[0]),
                   &call_result) == OM_STATUS_OK);
    OMI_ASSERT(call_result.value.abi_type == OM_NATIVE_ABI_CSIZE);
    OMI_ASSERT(call_result.value.as.unsigned_integer
               == sizeof(utf8_value) - 1);
    OMI_ASSERT(om_native_call_invoke(
                   &echo_string_binding, utf8_arguments,
                   sizeof(utf8_arguments) / sizeof(utf8_arguments[0]),
                   &call_result) == OM_STATUS_OK);
    OMI_ASSERT(call_result.value.kind == OM_NATIVE_VALUE_UTF8_STRING);
    OMI_ASSERT(call_result.value.as.utf8_string.length
               == sizeof(utf8_value) - 1);
    OMI_ASSERT(memcmp(call_result.value.as.utf8_string.data, utf8_value,
                      sizeof(utf8_value) - 1) == 0);
    om_native_call_result_destroy(&call_result);
    OMI_ASSERT(om_native_call_invoke(
                   &bounded_utf8_binding, utf8_arguments,
                   sizeof(utf8_arguments) / sizeof(utf8_arguments[0]),
                   &call_result) == OM_STATUS_OUT_OF_RANGE);
    utf8_arguments[0].as.utf8_string =
        (Om_String_View){invalid_utf8, sizeof(invalid_utf8)};
    OMI_ASSERT(om_native_call_invoke(
                   &utf8_binding, utf8_arguments,
                   sizeof(utf8_arguments) / sizeof(utf8_arguments[0]),
                   &call_result) == OM_STATUS_INVALID_UTF8);
    utf8_arguments[0].as.utf8_string =
        (Om_String_View){utf8_value, sizeof(utf8_value) - 1};
    OMI_ASSERT(om_native_call_invoke(
                   &i8_binding, out_of_range_i8,
                   sizeof(out_of_range_i8) / sizeof(out_of_range_i8[0]),
                   &call_result) == OM_STATUS_OUT_OF_RANGE);
    OMI_ASSERT(om_native_call_invoke(
                   &i8_binding, valid_i8,
                   sizeof(valid_i8) / sizeof(valid_i8[0]),
                   &call_result) == OM_STATUS_OK);
    OMI_ASSERT(call_result.value.as.signed_integer == -128);
    OMI_ASSERT(om_native_call_invoke(&owned_binding, NULL, 0,
                                     &call_result) == OM_STATUS_OK);
    OMI_ASSERT(call_result.ownership == OM_NATIVE_OWNERSHIP_OWNED);
    OMI_ASSERT(call_result.value.kind == OM_NATIVE_VALUE_POINTER);
    OMI_ASSERT(call_result.value.abi_type == OM_NATIVE_ABI_CSTRING);
    OMI_ASSERT(call_result.value.as.pointer != NULL);
    OMI_ASSERT(call_result.release_symbol.length
               == strlen("native_release_name"));
    OMI_ASSERT(om_native_call_result_release(&library, &call_result)
               == OM_STATUS_OK);
    OMI_ASSERT(call_result.value.as.pointer == NULL);
    OMI_ASSERT(om_native_library_lookup(
                   &library,
                   (Om_String_View){"missing_symbol", 14},
                   &symbol_address)
               == OM_STATUS_NOT_FOUND);
    OMI_ASSERT(symbol_address == NULL);
    point_arguments[0].as.record.size -= 1;
    OMI_ASSERT(om_native_call_invoke(
                   &echo_point_binding, point_arguments,
                   sizeof(point_arguments) / sizeof(point_arguments[0]),
                   &call_result) == OM_STATUS_INVALID_ARGUMENT);
    point_arguments[0].as.record.size = sizeof(point);
    OMI_ASSERT(om_native_call_invoke(
                   &echo_point_binding, point_arguments,
                   sizeof(point_arguments) / sizeof(point_arguments[0]),
                   &call_result) == OM_STATUS_OK);
    OMI_ASSERT(point.x == 13 && point.y == 2.5);
    OMI_ASSERT(call_result.value.kind == OM_NATIVE_VALUE_RECORD);
    OMI_ASSERT(call_result.value.abi_type == OM_NATIVE_ABI_RECORD);
    OMI_ASSERT(call_result.value.record_index == 0);
    OMI_ASSERT(call_result.value.as.record.size == sizeof(point));
    const struct Om_Test_Point *returned_point =
        call_result.value.as.record.data;
    OMI_ASSERT(returned_point != NULL);
    OMI_ASSERT(returned_point->x == 15 && returned_point->y == 7.5);
    passed = true;

cleanup:
    om_native_call_binding_destroy(&echo_point_binding);
    om_native_call_binding_destroy(&owned_binding);
    om_native_call_binding_destroy(&echo_string_binding);
    om_native_call_binding_destroy(&i8_binding);
    om_native_call_binding_destroy(&bounded_utf8_binding);
    om_native_call_binding_destroy(&utf8_binding);
    om_native_call_binding_destroy(&mixed_binding);
    om_native_call_binding_destroy(&add_binding);
    if (call_result.ownership == OM_NATIVE_OWNERSHIP_OWNED
        && call_result.value.kind == OM_NATIVE_VALUE_POINTER
        && call_result.value.as.pointer != NULL) {
        (void)om_native_call_result_release(&library, &call_result);
    }
    om_native_call_result_destroy(&call_result);
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    om_native_library_close(&library);
    return passed;
}

typedef struct Om_Native_Closure_Probe {
    atomic_int value;
    atomic_bool called;
} Om_Native_Closure_Probe;

static Om_Status native_closure_probe_entry(
    void *opaque, const Om_Native_Value *arguments, size_t argument_count)
{
    Om_Native_Closure_Probe *probe = opaque;
    if (probe == NULL || arguments == NULL || argument_count != 1
        || arguments[0].kind != OM_NATIVE_VALUE_SCALAR
        || arguments[0].abi_type != OM_NATIVE_ABI_CINT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&probe->value,
                          (int)arguments[0].as.signed_integer,
                          memory_order_relaxed);
    atomic_store_explicit(&probe->called, true, memory_order_release);
    return OM_STATUS_OK;
}

static bool test_omi_callback_closure_on_foreign_thread(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    static const char input[] =
        "NATIVE \"native_fixture\"\n"
        "FUNCTION invoke(callback: CALLBACK(CInt) -> CVoid): CVoid\n"
        "    SYMBOL \"om_ffi_invoke_single_callback_on_thread\"\n"
        "    EFFECT NATIVE\n"
        "    NONBLOCKING\n"
        "    CALLBACK callback FOREIGN_THREAD MESSAGE Tick\n"
        "END FUNCTION\n"
        "END NATIVE\n";
    static const char path[] = OM_NATIVE_FIXTURE_PATH;
    static const char source_path[] = "native_callback_test.omi";
    Om_Source_Text source = {0};
    Om_Omi_Module module = {0};
    Om_Native_Library library = {0};
    Om_Native_Call_Binding binding = {0};
    Om_Native_Callback_Closure closure = {0};
    Om_Native_Call_Result result = {0};
    Om_Native_Closure_Probe probe = {0};
    Om_Native_Value callback_argument = {0};
    bool passed = false;
    static const Om_Native_Callback_Entry entry = native_closure_probe_entry;

    atomic_init(&probe.value, 0);
    atomic_init(&probe.called, false);
    OMI_ASSERT(om_source_text_init(
                   &source, (Om_Source_Id){92},
                   (Om_String_View){source_path, sizeof(source_path) - 1},
                   (Om_Const_Byte_Slice){(const uint8_t *)input,
                                         sizeof(input) - 1}, 0)
               == OM_STATUS_OK);
    OMI_ASSERT(om_omi_parse(&source, &module) == OM_STATUS_OK);
    OMI_ASSERT(module.diagnostic_count == 0);
    OMI_ASSERT(om_native_library_open(
                   &library, (Om_String_View){path, sizeof(path) - 1})
               == OM_STATUS_OK);
    OMI_ASSERT(om_native_call_binding_init(
                   &binding, &library, &module, 0, 0) == OM_STATUS_OK);
    OMI_ASSERT(om_native_callback_closure_init(
                   &binding, 0, entry, &probe, &closure) == OM_STATUS_OK);
    callback_argument = (Om_Native_Value){
        .kind = OM_NATIVE_VALUE_POINTER,
        .abi_type = OM_NATIVE_ABI_CPOINTER,
        .as.pointer = om_native_callback_closure_code(&closure)
    };
    OMI_ASSERT(callback_argument.as.pointer != NULL);
    OMI_ASSERT(om_native_call_invoke(
                   &binding, &callback_argument, 1, &result) == OM_STATUS_OK);
    OMI_ASSERT(atomic_load_explicit(&probe.called, memory_order_acquire));
    OMI_ASSERT(atomic_load_explicit(&probe.value, memory_order_relaxed) == 42);
    passed = true;

cleanup:
    om_native_call_result_destroy(&result);
    om_native_callback_closure_destroy(&closure);
    om_native_call_binding_destroy(&binding);
    om_native_library_close(&library);
    om_omi_module_destroy(&module);
    om_source_text_destroy(&source);
    return passed;
}

static bool test_cstring_copy_and_owned_release(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    typedef char *(*Om_Native_Create_Name)(void);
    static const char path[] = OM_NATIVE_FIXTURE_PATH;
    static const char create_symbol[] = "native_create_name";
    static const char release_symbol[] = "native_release_name";
    static const char utf8_source[] = "name_\xc3\xa9";
    const char invalid_utf8[] = {(char)0xc3, 'x'};
    const char embedded_nul[] = {'a', '\0', 'b'};
    Om_Native_Library library = {0};
    Om_Native_C_String native_string = {0};
    Om_Native_C_String rejected_string = {0};
    Om_Native_String copied_string = {0};
    Om_Native_String owned_copy = {0};
    void *symbol_address = NULL;
    char *owned_pointer = NULL;
    Om_Native_Create_Name create_name = NULL;
    bool passed = false;
    OMI_ASSERT(om_native_cstring_from_utf8(
                   (Om_String_View){utf8_source, sizeof(utf8_source) - 1},
                   sizeof(utf8_source) - 1, &native_string)
               == OM_STATUS_OK);
    OMI_ASSERT(native_string.length == sizeof(utf8_source) - 1);
    OMI_ASSERT(native_string.data[native_string.length] == '\0');
    OMI_ASSERT(om_native_utf8_from_cstring(native_string.data,
                                           native_string.length,
                                           &copied_string)
               == OM_STATUS_OK);
    OMI_ASSERT(copied_string.length == sizeof(utf8_source) - 1);
    OMI_ASSERT(memcmp(copied_string.data, utf8_source,
                      copied_string.length) == 0);
    OMI_ASSERT(om_native_cstring_from_utf8(
                   (Om_String_View){embedded_nul, sizeof(embedded_nul)}, 8,
                   &rejected_string)
               == OM_STATUS_INVALID_ARGUMENT);
    OMI_ASSERT(om_native_cstring_from_utf8(
                   (Om_String_View){invalid_utf8, sizeof(invalid_utf8)}, 8,
                   &rejected_string)
               == OM_STATUS_INVALID_UTF8);
    OMI_ASSERT(om_native_cstring_from_utf8(
                   (Om_String_View){utf8_source, sizeof(utf8_source) - 1}, 2,
                   &rejected_string)
               == OM_STATUS_OUT_OF_RANGE);
    OMI_ASSERT(om_native_library_open(
                   &library, (Om_String_View){path, sizeof(path) - 1})
               == OM_STATUS_OK);
    OMI_ASSERT(om_native_library_lookup(
                   &library,
                   (Om_String_View){create_symbol, sizeof(create_symbol) - 1},
                   &symbol_address)
               == OM_STATUS_OK);
    OMI_ASSERT(sizeof(create_name) == sizeof(symbol_address));
    (void)memcpy(&create_name, &symbol_address, sizeof(create_name));
    owned_pointer = create_name();
    OMI_ASSERT(owned_pointer != NULL);
    OMI_ASSERT(om_native_utf8_from_cstring(owned_pointer, 64,
                                           &owned_copy)
               == OM_STATUS_OK);
    OMI_ASSERT(owned_copy.length == strlen("native-name"));
    OMI_ASSERT(memcmp(owned_copy.data, "native-name",
                      owned_copy.length) == 0);
    OMI_ASSERT(om_native_owned_pointer_release(
                   &library,
                   (Om_String_View){release_symbol,
                                    sizeof(release_symbol) - 1},
                   owned_pointer)
               == OM_STATUS_OK);
    owned_pointer = NULL;
    passed = true;

cleanup:
    if (owned_pointer != NULL && library.handle != NULL) {
        (void)om_native_owned_pointer_release(
            &library,
            (Om_String_View){release_symbol, sizeof(release_symbol) - 1},
            owned_pointer);
    }
    om_native_cstring_destroy(&native_string);
    om_native_cstring_destroy(&rejected_string);
    om_native_string_destroy(&copied_string);
    om_native_string_destroy(&owned_copy);
    om_native_library_close(&library);
    return passed;
}

typedef struct Om_Native_Callback_Probe {
    atomic_int value;
    atomic_bool called;
    atomic_bool ran_on_foreign_thread;
#if defined(_WIN32)
    DWORD caller_thread;
#else
    pthread_t caller_thread;
#endif
} Om_Native_Callback_Probe;

static void native_callback_probe(int value, void *opaque)
{
    Om_Native_Callback_Probe *probe = opaque;
    atomic_store_explicit(&probe->value, value, memory_order_relaxed);
    atomic_store_explicit(&probe->called, true, memory_order_release);
#if defined(_WIN32)
    const bool foreign_thread = GetCurrentThreadId() != probe->caller_thread;
#else
    const bool foreign_thread =
        pthread_equal(pthread_self(), probe->caller_thread) == 0;
#endif
    atomic_store_explicit(&probe->ran_on_foreign_thread, foreign_thread,
                          memory_order_release);
}

static bool test_callback_runs_on_foreign_thread(void)
{
#ifndef OM_NATIVE_FIXTURE_PATH
#error "CMake must provide the native fixture path"
#endif
    typedef int (*Om_Native_Invoke_Callback)(void (*)(int, void *), void *);
    static const char path[] = OM_NATIVE_FIXTURE_PATH;
    static const char symbol_name[] = "om_ffi_invoke_callback_on_thread";
    Om_Native_Library library = {0};
    Om_Native_Callback_Probe probe = {0};
    void *symbol_address = NULL;
    Om_Native_Invoke_Callback invoke = NULL;
    bool passed = false;
#if defined(_WIN32)
    probe.caller_thread = GetCurrentThreadId();
#else
    probe.caller_thread = pthread_self();
#endif
    OMI_ASSERT(om_native_library_open(
                   &library, (Om_String_View){path, sizeof(path) - 1})
               == OM_STATUS_OK);
    OMI_ASSERT(om_native_library_lookup(
                   &library,
                   (Om_String_View){symbol_name, sizeof(symbol_name) - 1},
                   &symbol_address)
               == OM_STATUS_OK);
    OMI_ASSERT(sizeof(invoke) == sizeof(symbol_address));
    (void)memcpy(&invoke, &symbol_address, sizeof(invoke));
    OMI_ASSERT(invoke != NULL && invoke(native_callback_probe, &probe) == 0);
    OMI_ASSERT(atomic_load_explicit(&probe.called, memory_order_acquire));
    OMI_ASSERT(atomic_load_explicit(&probe.value, memory_order_relaxed) == 42);
    OMI_ASSERT(atomic_load_explicit(&probe.ran_on_foreign_thread,
                                    memory_order_acquire));
    passed = true;

cleanup:
    om_native_library_close(&library);
    return passed;
}

static bool test_cpp_extern_c_wrapper(void)
{
#ifndef OM_CPP_NATIVE_FIXTURE_PATH
#error "CMake must provide the C++ native fixture path"
#endif
    typedef int (*Om_Native_Cpp_Transform)(int);
    static const char path[] = OM_CPP_NATIVE_FIXTURE_PATH;
    static const char symbol_name[] = "om_ffi_cpp_transform";
    Om_Native_Library library = {0};
    void *symbol_address = NULL;
    Om_Native_Cpp_Transform transform = NULL;
    bool passed = false;
    OMI_ASSERT(om_native_library_open(
                   &library, (Om_String_View){path, sizeof(path) - 1})
               == OM_STATUS_OK);
    OMI_ASSERT(om_native_library_lookup(
                   &library,
                   (Om_String_View){symbol_name, sizeof(symbol_name) - 1},
                   &symbol_address)
               == OM_STATUS_OK);
    OMI_ASSERT(sizeof(transform) == sizeof(symbol_address));
    (void)memcpy(&transform, &symbol_address, sizeof(transform));
    OMI_ASSERT(transform != NULL);
    OMI_ASSERT(transform(21) == 42);
    OMI_ASSERT(transform(-1) == -1);
    passed = true;

cleanup:
    om_native_library_close(&library);
    return passed;
}

static bool test_omi_malformed_input_fuzz(void)
{
    static const char alphabet[] =
        "NATIVE FUNCTION RECORD END EFFECT SYMBOL BLOCKING NONBLOCKING "
        "RETURNS CALLBACK BORROWED OWNED STATIC CInt CVoid CPointer CString "
        "NATIVE_RECORD(),:->'\"\\ abcxyz\n\r\t";
    uint32_t random_state = UINT32_C(0x8f31a2c7);
    for (uint32_t round = 0; round < UINT32_C(512); ++round) {
        char input[96] = {0};
        random_state = random_state * UINT32_C(1664525)
            + UINT32_C(1013904223);
        const size_t length = (size_t)(random_state % (uint32_t)sizeof(input));
        for (size_t index = 0; index < length; ++index) {
            random_state = random_state * UINT32_C(1664525)
                + UINT32_C(1013904223);
            input[index] = alphabet[(random_state >> 16)
                % (uint32_t)(sizeof(alphabet) - 1)];
        }
        Om_Source_Text source = {0};
        Om_Omi_Module module = {0};
        static const char path[] = "omi_fuzz.omi";
        const Om_Status source_status = om_source_text_init(
            &source, (Om_Source_Id){round + UINT32_C(1000)},
            (Om_String_View){path, sizeof(path) - 1},
            (Om_Const_Byte_Slice){(const uint8_t *)input, length}, 0);
        if (source_status != OM_STATUS_OK
            || om_omi_parse(&source, &module) != OM_STATUS_OK) {
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

static bool test_omi_project_loads_explicit_metadata_paths(void)
{
    static const char root_path[] = OM_OMI_PROJECT_TEST_ROOT;
    static const Om_String_View paths[] = {
        {"math.omi", sizeof("math.omi") - 1}
    };
    Om_Source_Root root = {0};
    Om_Omi_Project project = {0};
    bool passed = false;

    OMI_ASSERT(om_source_root_init(
        &root, (Om_String_View){root_path, sizeof(root_path) - 1})
        == OM_STATUS_OK);
    OMI_ASSERT(om_omi_project_load(
        &root, paths, sizeof(paths) / sizeof(paths[0]), 7000, 0, &project)
        == OM_STATUS_OK);
    OMI_ASSERT(project.module_count == 1);
    const Om_Omi_Module *module = om_omi_project_module(&project, 0);
    const Om_Source_Text *source = om_omi_project_source(&project, 0);
    OMI_ASSERT(module != NULL && source != NULL);
    OMI_ASSERT(module->diagnostic_count == 0);
    OMI_ASSERT(module->function_count == 1);
    OMI_ASSERT(om_string_view_equal(
        module->functions[0].name, (Om_String_View){"add", 3}));
    OMI_ASSERT(om_string_view_equal(
        source->canonical_path, (Om_String_View){"math.omi", 8}));
    passed = true;

cleanup:
    om_omi_project_destroy(&project);
    om_source_root_destroy(&root);
    return passed;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"omi_complete_native_metadata", test_complete_native_metadata},
        {"omi_invalid_ownership", test_invalid_ownership_metadata},
        {"omi_callback_and_syntax_diagnostics",
         test_callback_and_syntax_diagnostics},
        {"native_abi_type_and_record_layout",
         test_c_abi_type_and_record_layout},
        {"native_dynamic_c_roundtrip", test_dynamic_c_roundtrip},
        {"native_omi_callback_closure",
         test_omi_callback_closure_on_foreign_thread},
        {"native_cstring_and_owned_release",
         test_cstring_copy_and_owned_release},
        {"native_callback_foreign_thread",
         test_callback_runs_on_foreign_thread},
        {"native_cpp_extern_c_wrapper", test_cpp_extern_c_wrapper},
        {"omi_project_explicit_metadata_paths",
         test_omi_project_loads_explicit_metadata_paths},
        {"omi_malformed_input_fuzz", test_omi_malformed_input_fuzz}
    };
    size_t failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (tests[index].function()) {
            (void)printf("PASS %s\n", tests[index].name);
        } else {
            (void)fprintf(stderr, "FAIL %s\n", tests[index].name);
            ++failures;
        }
    }
    (void)printf("%zu tests, %zu failures\n",
                 sizeof(tests) / sizeof(tests[0]), failures);
    return failures == 0 ? 0 : 1;
}
