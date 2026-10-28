#ifndef OMBASIC_NATIVE_METADATA_OMI_H
#define OMBASIC_NATIVE_METADATA_OMI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/source/source_text.h"

typedef enum Om_Native_Abi_Type {
    OM_NATIVE_ABI_INVALID = 0,
    OM_NATIVE_ABI_CVOID,
    OM_NATIVE_ABI_CINT8,
    OM_NATIVE_ABI_CINT16,
    OM_NATIVE_ABI_CINT32,
    OM_NATIVE_ABI_CINT64,
    OM_NATIVE_ABI_CUINT8,
    OM_NATIVE_ABI_CUINT16,
    OM_NATIVE_ABI_CUINT32,
    OM_NATIVE_ABI_CUINT64,
    OM_NATIVE_ABI_CINT,
    OM_NATIVE_ABI_CUINT,
    OM_NATIVE_ABI_CLONG,
    OM_NATIVE_ABI_CULONG,
    OM_NATIVE_ABI_CLONGLONG,
    OM_NATIVE_ABI_CULONGLONG,
    OM_NATIVE_ABI_CFLOAT,
    OM_NATIVE_ABI_CDOUBLE,
    OM_NATIVE_ABI_CSIZE,
    OM_NATIVE_ABI_CBOOL,
    OM_NATIVE_ABI_CPOINTER,
    OM_NATIVE_ABI_CSTRING,
    /* A by-value type resolved through an NATIVE RECORD declaration. */
    OM_NATIVE_ABI_RECORD
} Om_Native_Abi_Type;

typedef enum Om_Native_Ownership {
    OM_NATIVE_OWNERSHIP_NONE = 0,
    OM_NATIVE_OWNERSHIP_BORROWED,
    OM_NATIVE_OWNERSHIP_OWNED,
    OM_NATIVE_OWNERSHIP_STATIC
} Om_Native_Ownership;

typedef enum Om_Native_Blocking {
    OM_NATIVE_BLOCKING_UNSPECIFIED = 0,
    OM_NATIVE_BLOCKING_YES,
    OM_NATIVE_BLOCKING_NO
} Om_Native_Blocking;

typedef enum Om_Native_Effect {
    OM_NATIVE_EFFECT_NONE = 0,
    OM_NATIVE_EFFECT_NATIVE = 1u << 0,
    OM_NATIVE_EFFECT_FILE_IO = 1u << 1,
    OM_NATIVE_EFFECT_NETWORK_IO = 1u << 2,
    OM_NATIVE_EFFECT_CLOCK = 1u << 3
} Om_Native_Effect;

typedef enum Om_Omi_Diagnostic_Code {
    OM_OMI_DIAGNOSTIC_E0801 = 801,
    OM_OMI_DIAGNOSTIC_E0802 = 802,
    OM_OMI_DIAGNOSTIC_E0803 = 803,
    OM_OMI_DIAGNOSTIC_E0804 = 804,
    OM_OMI_DIAGNOSTIC_E0805 = 805,
    OM_OMI_DIAGNOSTIC_E0806 = 806,
    OM_OMI_DIAGNOSTIC_E0807 = 807,
    OM_OMI_DIAGNOSTIC_E0808 = 808
} Om_Omi_Diagnostic_Code;

typedef struct Om_Omi_Diagnostic {
    Om_Omi_Diagnostic_Code code;
    Om_Source_Span span;
} Om_Omi_Diagnostic;

typedef struct Om_Omi_Callback_Parameter {
    Om_Native_Abi_Type abi_type;
    Om_Source_Span span;
} Om_Omi_Callback_Parameter;

typedef struct Om_Omi_Parameter {
    Om_String_View name;
    Om_Native_Abi_Type abi_type;
    size_t record_index;
    Om_String_View record_name;
    Om_Native_Ownership ownership;
    bool is_callback;
    Om_Native_Abi_Type callback_result_type;
    size_t callback_parameter_start;
    size_t callback_parameter_count;
    Om_Source_Span span;
} Om_Omi_Parameter;

typedef struct Om_Omi_Callback_Binding {
    size_t parameter_index;
    Om_String_View message_name;
    Om_Source_Span span;
} Om_Omi_Callback_Binding;

typedef struct Om_Omi_Native_Function {
    Om_String_View name;
    Om_String_View symbol;
    Om_Native_Abi_Type result_type;
    size_t result_record_index;
    Om_String_View result_record_name;
    Om_Source_Span result_type_span;
    uint32_t effects;
    Om_Native_Blocking blocking;
    Om_Native_Ownership result_ownership;
    Om_String_View result_owner_parameter;
    Om_String_View release_function;
    size_t parameter_start;
    size_t parameter_count;
    size_t callback_binding_start;
    size_t callback_binding_count;
    Om_Source_Span span;
} Om_Omi_Native_Function;

typedef struct Om_Omi_Record_Field {
    Om_String_View name;
    Om_Native_Abi_Type abi_type;
    Om_Native_Ownership ownership;
    Om_Source_Span span;
} Om_Omi_Record_Field;

typedef struct Om_Omi_Native_Record {
    Om_String_View name;
    size_t field_start;
    size_t field_count;
    Om_Source_Span span;
} Om_Omi_Native_Record;

typedef struct Om_Omi_Module {
    Om_String_View library_path;
    const Om_Omi_Native_Function *functions;
    size_t function_count;
    const Om_Omi_Parameter *parameters;
    size_t parameter_count;
    const Om_Omi_Callback_Parameter *callback_parameters;
    size_t callback_parameter_count;
    const Om_Omi_Callback_Binding *callback_bindings;
    size_t callback_binding_count;
    const Om_Omi_Native_Record *records;
    size_t record_count;
    const Om_Omi_Record_Field *record_fields;
    size_t record_field_count;
    const Om_Omi_Diagnostic *diagnostics;
    size_t diagnostic_count;
    /* Owns result arrays and decoded string literals. */
    void *owned_storage;
} Om_Omi_Module;

/*
 * Pass a zero-initialized module. Identifiers borrow the source bytes; decoded
 * library/symbol strings and result arrays belong to the module. The source
 * must outlive the module.
 * Syntax and metadata errors are returned in module->diagnostics; a nonzero
 * diagnostic_count means the module must not be loaded.
 */
Om_Status om_omi_parse(const Om_Source_Text *source, Om_Omi_Module *module);
void om_omi_module_destroy(Om_Omi_Module *module);
const char *om_omi_diagnostic_code_name(Om_Omi_Diagnostic_Code code);

#endif
