#ifndef OMBASIC_FFI_NATIVE_CALL_H
#define OMBASIC_FFI_NATIVE_CALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/ffi/dynamic_library.h"
#include "ombasic/ffi/omi.h"

typedef enum Om_Native_Value_Kind {
    OM_NATIVE_VALUE_INVALID = 0,
    OM_NATIVE_VALUE_SCALAR,
    OM_NATIVE_VALUE_UTF8_STRING,
    OM_NATIVE_VALUE_POINTER,
    OM_NATIVE_VALUE_RECORD
} Om_Native_Value_Kind;

typedef struct Om_Native_Value {
    Om_Native_Value_Kind kind;
    Om_Native_Abi_Type abi_type;
    size_t record_index;
    union {
        int64_t signed_integer;
        uint64_t unsigned_integer;
        double floating_point;
        bool boolean;
        void *pointer;
        Om_String_View utf8_string;
        struct {
            const void *data;
            size_t size;
        } record;
    } as;
} Om_Native_Value;

typedef struct Om_Native_Call_Binding {
    /* Owns the prepared ABI call interface; initialized by the binding API. */
    void *owned_storage;
} Om_Native_Call_Binding;

typedef struct Om_Native_Call_Signature {
    Om_Native_Abi_Type result_type;
    size_t result_record_index;
    Om_Native_Ownership result_ownership;
    Om_Native_Blocking blocking;
    const Om_Native_Abi_Type *parameter_types;
    const Om_Omi_Parameter *parameters;
    const Om_Omi_Callback_Parameter *callback_parameters;
    size_t callback_parameter_count;
    size_t parameter_count;
} Om_Native_Call_Signature;

typedef struct Om_Native_Callback_Closure {
    void *owned_storage;
} Om_Native_Callback_Closure;

typedef Om_Status (*Om_Native_Callback_Entry)(
    void *context,
    const Om_Native_Value *arguments,
    size_t argument_count
);

#define OM_NATIVE_MAX_CALLBACK_PARAMETERS ((size_t)64)

typedef struct Om_Native_Call_Result {
    Om_Native_Value value;
    Om_Native_Ownership ownership;
    bool has_owner_parameter;
    size_t owner_parameter_index;
    /* Borrowed from the binding; valid until that binding is destroyed. */
    Om_String_View release_symbol;
    /* Owns copied CString or Native Record result storage, if non-NULL. */
    void *owned_copy_storage;
} Om_Native_Call_Result;

#define OM_NATIVE_DEFAULT_MAX_CSTRING_BYTES ((size_t)1024 * (size_t)1024)

/*
 * Bind one validated `.omi` declaration to a symbol from an open library.
 * The library and metadata module must outlive the binding. A zero string
 * limit selects OM_NATIVE_DEFAULT_MAX_CSTRING_BYTES.
 */
Om_Status om_native_call_binding_init(
    Om_Native_Call_Binding *binding,
    const Om_Native_Library *library,
    const Om_Omi_Module *module,
    size_t function_index,
    size_t maximum_cstring_bytes
);
/* Returned parameter types are borrowed from the binding. */
Om_Status om_native_call_binding_signature(
    const Om_Native_Call_Binding *binding,
    Om_Native_Call_Signature *signature
);
/*
 * Builds a temporary libffi closure for an `.omi` callback parameter.
 * Callback results must be CVoid. The closure and context are valid only until
 * destroyed, normally immediately after the enclosing Native call returns.
 */
Om_Status om_native_callback_closure_init(
    const Om_Native_Call_Binding *binding,
    size_t parameter_index,
    Om_Native_Callback_Entry entry,
    void *context,
    Om_Native_Callback_Closure *closure
);
void *om_native_callback_closure_code(
    const Om_Native_Callback_Closure *closure
);
void om_native_callback_closure_destroy(
    Om_Native_Callback_Closure *closure
);
void om_native_call_binding_destroy(Om_Native_Call_Binding *binding);

/*
 * Calls according to the `.omi` ABI signature. UTF8_STRING arguments are
 * copied to temporary NUL-terminated buffers. Native Record arguments are
 * copied to aligned temporary storage; RECORD results are owned by `result`
 * until it is destroyed. Borrowed CString results are copied before temporary
 * buffers are released. OWNED pointer arguments are cleared in `arguments`
 * after the native function is entered. `result` must be zero-initialized or
 * destroyed before reuse.
 */
Om_Status om_native_call_invoke(
    const Om_Native_Call_Binding *binding,
    Om_Native_Value *arguments,
    size_t argument_count,
    Om_Native_Call_Result *result
);

/* Releases a non-null Owned pointer/CString result through its `.omi` symbol. */
Om_Status om_native_call_result_release(
    const Om_Native_Library *library,
    Om_Native_Call_Result *result
);

/* Frees any copied Borrowed CString storage and clears the result value. */
void om_native_call_result_destroy(Om_Native_Call_Result *result);

/* Releases an Owned pointer/CString through this binding's open library. */
Om_Status om_native_call_result_release_bound(
    const Om_Native_Call_Binding *binding,
    Om_Native_Call_Result *result
);

#endif
