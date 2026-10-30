#include "backend_internal.h"

static bool native_target_uses_windows_long(const Om_Llvm_Backend_Context *backend);
static bool native_target_is_windows_x64(const Om_Llvm_Backend_Context *backend);
static bool native_target_is_sysv_amd64(const Om_Llvm_Backend_Context *backend);
static bool native_target_is_darwin_arm64(const Om_Llvm_Backend_Context *backend);
static bool native_target_is_aapcs64(const Om_Llvm_Backend_Context *backend);
static Om_Status native_record_abi(Om_Llvm_Backend_Context *backend,
                                   const Om_Omi_Module *module,
                                   size_t record_index,
                                   Om_Llvm_Native_Record_Abi *abi);
static Om_Status native_record_return_type(Om_Llvm_Backend_Context *backend,
                                           const Om_Llvm_Native_Record_Abi *abi,
                                           LLVMTypeRef *type);
static Om_Status add_native_type_attribute(Om_Llvm_Backend_Context *backend,
                                           LLVMValueRef function,
                                           LLVMValueRef call,
                                           LLVMAttributeIndex index,
                                           const char *name,
                                           LLVMTypeRef type);
static Om_Status native_record_part_address(Om_Llvm_Backend_Context *backend,
                                           LLVMValueRef slot,
                                           const Om_Llvm_Native_Record_Part *part,
                                           LLVMValueRef *address);
static Om_Status load_native_record_part(Om_Llvm_Backend_Context *backend,
                                         LLVMValueRef slot,
                                         const Om_Llvm_Native_Record_Part *part,
                                         LLVMValueRef *value);
static Om_Status store_native_record_part(Om_Llvm_Backend_Context *backend,
                                          LLVMValueRef slot,
                                          const Om_Llvm_Native_Record_Part *part,
                                          LLVMValueRef value);
static Om_Status declare_native_external(Om_Llvm_Backend_Context *backend,
                                         Om_String_View symbol,
                                         LLVMTypeRef signature,
                                         LLVMValueRef *function_out);
static Om_Status native_release_function(
    Om_Llvm_Backend_Context *backend,
    const Om_Omi_Module *module,
    Om_String_View release_name,
    LLVMValueRef *release_function_out);
static Om_Status native_pointer_result(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Module *module,
    const Om_Omi_Native_Function *native_function,
    const Om_Mir_Value_Id *operands,
    LLVMValueRef native_result,
    LLVMValueRef *managed_result);
static Om_Status native_cstring_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument);
static Om_Status native_utf8_slice_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument);
static Om_Status native_pointer_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument);
static Om_Status native_scalar_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    const Om_Llvm_Native_Abi_Type *abi_type,
    LLVMValueRef value,
    LLVMValueRef *argument);
static Om_Status emit_native_record_call(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Mir_Instruction *instruction,
    const Om_Mir_Value_Id *operands,
    LLVMValueRef *result,
    const Om_Symbol *symbol,
    const Om_Omi_Native_Function *native_function);

typedef struct Om_Llvm_Native_Call_Parameter {
    Om_Llvm_Native_Abi_Type scalar;
    Om_Llvm_Native_Record_Abi record;
    size_t abi_start;
    size_t abi_count;
    bool is_record;
} Om_Llvm_Native_Call_Parameter;

static bool native_target_uses_windows_long(const Om_Llvm_Backend_Context *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_WIN64_X64;
}

Om_Status native_abi_type(Om_Llvm_Backend_Context *backend,
                                 Om_Native_Abi_Type abi_type,
                                 bool is_result,
                                 Om_Llvm_Native_Abi_Type *result)
{
    if (backend == NULL || backend->target_data == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned pointer_bytes = LLVMPointerSize(backend->target_data);
    if (pointer_bytes == 0 || pointer_bytes > UINT_MAX / CHAR_BIT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned pointer_bits = pointer_bytes * CHAR_BIT;
    unsigned bits = 0;
    bool is_integer = true;
    bool is_signed = false;
    bool is_float = false;
    bool is_boolean = false;
    switch (abi_type) {
    case OM_NATIVE_ABI_CVOID:
        if (!is_result) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *result = (Om_Llvm_Native_Abi_Type){
            .llvm_type = LLVMVoidTypeInContext(backend->context),
        };
        return OM_STATUS_OK;
    case OM_NATIVE_ABI_CINT8: bits = 8; is_signed = true; break;
    case OM_NATIVE_ABI_CINT16: bits = 16; is_signed = true; break;
    case OM_NATIVE_ABI_CINT32: bits = 32; is_signed = true; break;
    case OM_NATIVE_ABI_CINT64: bits = 64; is_signed = true; break;
    case OM_NATIVE_ABI_CUINT8: bits = 8; break;
    case OM_NATIVE_ABI_CUINT16: bits = 16; break;
    case OM_NATIVE_ABI_CUINT32: bits = 32; break;
    case OM_NATIVE_ABI_CUINT64: bits = 64; break;
    case OM_NATIVE_ABI_CINT: bits = 32; is_signed = true; break;
    case OM_NATIVE_ABI_CUINT: bits = 32; break;
    case OM_NATIVE_ABI_CLONG:
        bits = native_target_uses_windows_long(backend) ? 32 : pointer_bits;
        is_signed = true;
        break;
    case OM_NATIVE_ABI_CULONG:
        bits = native_target_uses_windows_long(backend) ? 32 : pointer_bits;
        break;
    case OM_NATIVE_ABI_CLONGLONG: bits = 64; is_signed = true; break;
    case OM_NATIVE_ABI_CULONGLONG: bits = 64; break;
    case OM_NATIVE_ABI_CSIZE: bits = pointer_bits; break;
    case OM_NATIVE_ABI_CBOOL:
        bits = 1;
        is_boolean = true;
        break;
    case OM_NATIVE_ABI_CFLOAT:
        bits = 32;
        is_integer = false;
        is_float = true;
        break;
    case OM_NATIVE_ABI_CDOUBLE:
        bits = 64;
        is_integer = false;
        is_float = true;
        break;
    case OM_NATIVE_ABI_CSTRING:
    case OM_NATIVE_ABI_CPOINTER:
    case OM_NATIVE_ABI_CUTF8_SLICE:
        *result = (Om_Llvm_Native_Abi_Type){
            .llvm_type = backend->byte_pointer_type,
            .bit_width = pointer_bits,
            .is_pointer = true,
        };
        return OM_STATUS_OK;
    case OM_NATIVE_ABI_INVALID:
    case OM_NATIVE_ABI_RECORD:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef llvm_type = NULL;
    if (is_float) {
        llvm_type = bits == 32
            ? LLVMFloatTypeInContext(backend->context)
            : LLVMDoubleTypeInContext(backend->context);
    } else if (bits == 1) {
        llvm_type = backend->i1_type;
    } else if (bits == 8) {
        llvm_type = backend->i8_type;
    } else if (bits == 32) {
        llvm_type = backend->i32_type;
    } else if (bits == 64) {
        llvm_type = backend->i64_type;
    } else if (bits != 0) {
        llvm_type = LLVMIntTypeInContext(backend->context, bits);
    }
    if (llvm_type == NULL || bits == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = (Om_Llvm_Native_Abi_Type){
        .llvm_type = llvm_type,
        .bit_width = bits,
        .is_integer = is_integer,
        .is_signed = is_signed,
        .is_float = is_float,
        .is_boolean = is_boolean,
    };
    return OM_STATUS_OK;
}

static bool native_target_is_windows_x64(const Om_Llvm_Backend_Context *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_WIN64_X64;
}

static bool native_target_is_sysv_amd64(const Om_Llvm_Backend_Context *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_SYSV_AMD64;
}

static bool native_target_is_darwin_arm64(const Om_Llvm_Backend_Context *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_DARWIN_ARM64;
}

static bool native_target_is_aapcs64(const Om_Llvm_Backend_Context *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_AAPCS64;
}

static Om_Status native_record_abi(Om_Llvm_Backend_Context *backend,
                                   const Om_Omi_Module *module,
                                   size_t record_index,
                                   Om_Llvm_Native_Record_Abi *abi)
{
    if (backend == NULL || module == NULL || abi == NULL
        || record_index >= module->record_count || module->records == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef record_type = NULL;
    Om_Status status = native_record_llvm_type(backend, module, record_index,
                                                &record_type);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const uint64_t size = LLVMABISizeOfType(backend->target_data, record_type);
    const uint64_t alignment = LLVMABIAlignmentOfType(backend->target_data,
                                                       record_type);
    if (size == 0 || alignment == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (native_target_is_windows_x64(backend)) {
        return om_llvm_native_record_classify_win64(backend->context,
            record_type, size, alignment, abi);
    }
    const bool is_sysv_amd64 = native_target_is_sysv_amd64(backend);
    const bool is_aapcs64 = native_target_is_aapcs64(backend);
    const bool is_darwin_arm64 = native_target_is_darwin_arm64(backend);
    if (!is_sysv_amd64 && !is_aapcs64 && !is_darwin_arm64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Native_Record *record = &module->records[record_index];
    if (record->field_start > module->record_field_count
        || record->field_count
            > module->record_field_count - record->field_start
        || record->field_count == 0 || module->record_fields == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Llvm_Native_Record_Field_Layout *fields = om_alloc_array(
        record->field_count, sizeof(*fields));
    if (fields == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < record->field_count; ++index) {
        const Om_Omi_Record_Field *field = &module->record_fields[
            record->field_start + index];
        Om_Llvm_Native_Abi_Type field_abi = {0};
        if (field->abi_type == OM_NATIVE_ABI_CBOOL) {
            field_abi.llvm_type = backend->i8_type;
            field_abi.bit_width = 8;
            field_abi.is_integer = true;
        } else {
            status = native_abi_type(backend, field->abi_type, false,
                                     &field_abi);
        }
        if (status == OM_STATUS_OK) {
            fields[index] = (Om_Llvm_Native_Record_Field_Layout){
                .llvm_type = field_abi.llvm_type,
                .offset = LLVMOffsetOfElement(backend->target_data,
                    record_type, (unsigned)index),
                .size = LLVMABISizeOfType(backend->target_data,
                                           field_abi.llvm_type),
                .alignment = LLVMABIAlignmentOfType(backend->target_data,
                                                      field_abi.llvm_type),
                .bit_width = field_abi.bit_width,
                .is_integer = field_abi.is_integer,
                .is_signed = field_abi.is_signed,
                .is_float = field_abi.is_float,
                .is_pointer = field_abi.is_pointer,
            };
        }
    }
    if (status == OM_STATUS_OK) {
        if (is_aapcs64) {
            status = om_llvm_native_record_classify_aapcs64(
                backend->context, record_type, size, alignment, fields,
                record->field_count, abi);
        } else if (is_darwin_arm64) {
            status = om_llvm_native_record_classify_darwin_arm64(
                backend->context, record_type, size, alignment, fields,
                record->field_count, abi);
        } else {
            status = om_llvm_native_record_classify_sysv_amd64(
                backend->context, record_type, size, alignment, fields,
                record->field_count, abi);
        }
    }
    om_free(fields);
    return status;
}

static Om_Status native_record_return_type(Om_Llvm_Backend_Context *backend,
                                           const Om_Llvm_Native_Record_Abi *abi,
                                           LLVMTypeRef *type)
{
    if (backend == NULL || abi == NULL || type == NULL
        || abi->indirect || abi->part_count == 0 || abi->part_count > 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (abi->result_type != NULL) {
        *type = abi->result_type;
        return OM_STATUS_OK;
    }
    if (abi->result_is_record) {
        *type = abi->llvm_type;
        return OM_STATUS_OK;
    }
    if (abi->part_count == 1) {
        *type = abi->parts[0].llvm_type;
        return OM_STATUS_OK;
    }
    LLVMTypeRef parts[2] = {
        abi->parts[0].llvm_type,
        abi->parts[1].llvm_type,
    };
    *type = LLVMStructTypeInContext(backend->context, parts, 2, 0);
    return *type == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

Om_Status add_native_enum_attribute(Om_Llvm_Backend_Context *backend,
                                           LLVMValueRef function,
                                           LLVMValueRef call,
                                           LLVMAttributeIndex index,
                                           const char *name,
                                           uint64_t value)
{
    if (backend == NULL || name == NULL
        || (function == NULL && call == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (kind == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMAttributeRef attribute = LLVMCreateEnumAttribute(backend->context,
                                                          kind, value);
    if (attribute == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (function != NULL) {
        LLVMAddAttributeAtIndex(function, index, attribute);
    }
    if (call != NULL) {
        LLVMAddCallSiteAttribute(call, index, attribute);
    }
    return OM_STATUS_OK;
}

static Om_Status add_native_type_attribute(Om_Llvm_Backend_Context *backend,
                                           LLVMValueRef function,
                                           LLVMValueRef call,
                                           LLVMAttributeIndex index,
                                           const char *name,
                                           LLVMTypeRef type)
{
    if (backend == NULL || name == NULL || type == NULL
        || (function == NULL && call == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (kind == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMAttributeRef attribute = LLVMCreateTypeAttribute(backend->context,
                                                          kind, type);
    if (attribute == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (function != NULL) {
        LLVMAddAttributeAtIndex(function, index, attribute);
    }
    if (call != NULL) {
        LLVMAddCallSiteAttribute(call, index, attribute);
    }
    return OM_STATUS_OK;
}

static Om_Status native_record_part_address(Om_Llvm_Backend_Context *backend,
                                           LLVMValueRef slot,
                                           const Om_Llvm_Native_Record_Part *part,
                                           LLVMValueRef *address)
{
    if (backend == NULL || slot == NULL || part == NULL
        || (part->memory_type == NULL && part->llvm_type == NULL)
        || address == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef base = LLVMBuildBitCast(backend->builder, slot,
        backend->byte_pointer_type, "native_record_bytes");
    LLVMTypeRef size_type = LLVMIntPtrTypeInContext(backend->context,
                                                     backend->target_data);
    LLVMValueRef offset = LLVMConstInt(size_type, part->offset, 0);
    LLVMValueRef bytes = base == NULL || size_type == NULL ? NULL
        : LLVMBuildGEP2(backend->builder, backend->i8_type, base, &offset, 1,
                        "native_record_part_bytes");
    LLVMTypeRef memory_type = part->memory_type != NULL
        ? part->memory_type : part->llvm_type;
    LLVMTypeRef pointer_type = LLVMPointerType(memory_type, 0);
    *address = bytes == NULL ? NULL : LLVMBuildBitCast(backend->builder,
        bytes, pointer_type, "native_record_part_address");
    return *address == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static Om_Status load_native_record_part(Om_Llvm_Backend_Context *backend,
                                         LLVMValueRef slot,
                                         const Om_Llvm_Native_Record_Part *part,
                                         LLVMValueRef *value)
{
    if (backend == NULL || slot == NULL || part == NULL || value == NULL
        || part->llvm_type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (part->mode == OM_LLVM_NATIVE_RECORD_PART_INTEGER_ARRAY) {
        if (LLVMGetTypeKind(part->llvm_type) != LLVMArrayTypeKind
            || part->memory_size == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMTypeRef element_type = LLVMGetElementType(part->llvm_type);
        if (element_type == NULL || LLVMGetTypeKind(element_type)
                != LLVMIntegerTypeKind) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const unsigned element_bits = LLVMGetIntTypeWidth(element_type);
        const uint64_t element_bytes = element_bits / CHAR_BIT;
        const unsigned element_count = LLVMGetArrayLength(part->llvm_type);
        if (element_bytes == 0 || element_count == 0
            || part->memory_size > element_bytes * element_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef aggregate = LLVMConstNull(part->llvm_type);
        if (aggregate == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        uint64_t consumed = 0;
        for (unsigned index = 0; index < element_count; ++index) {
            const uint64_t remaining = part->memory_size - consumed;
            if (remaining == 0) {
                break;
            }
            const uint64_t chunk_size = remaining < element_bytes
                ? remaining : element_bytes;
            if (chunk_size > UINT_MAX / CHAR_BIT) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            LLVMTypeRef chunk_type = LLVMIntTypeInContext(backend->context,
                (unsigned)(chunk_size * CHAR_BIT));
            if (chunk_type == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            Om_Llvm_Native_Record_Part chunk = *part;
            chunk.llvm_type = chunk_type;
            chunk.memory_type = chunk_type;
            chunk.offset = part->offset + consumed;
            chunk.memory_size = chunk_size;
            chunk.alignment = part->alignment;
            chunk.mode = OM_LLVM_NATIVE_RECORD_PART_DIRECT;
            LLVMValueRef chunk_value = NULL;
            Om_Status status = load_native_record_part(backend, slot, &chunk,
                                                        &chunk_value);
            if (status != OM_STATUS_OK) {
                return status;
            }
            const unsigned chunk_bits =
                LLVMGetIntTypeWidth(LLVMTypeOf(chunk_value));
            LLVMValueRef widened = chunk_value;
            if (chunk_bits < element_bits) {
                widened = LLVMBuildZExt(backend->builder, chunk_value,
                    element_type, "native_record_chunk_extend");
            } else if (chunk_bits > element_bits) {
                widened = LLVMBuildTrunc(backend->builder, chunk_value,
                    element_type, "native_record_chunk_truncate");
            }
            if (widened == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            aggregate = LLVMBuildInsertValue(backend->builder, aggregate,
                widened, index, "native_record_array_part");
            if (aggregate == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            consumed += chunk_size;
        }
        if (consumed != part->memory_size) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *value = aggregate;
        return OM_STATUS_OK;
    }

    LLVMValueRef address = NULL;
    Om_Status status = native_record_part_address(backend, slot, part,
                                                   &address);
    if (status == OM_STATUS_OK) {
        LLVMTypeRef memory_type = part->memory_type != NULL
            ? part->memory_type : part->llvm_type;
        LLVMValueRef loaded = LLVMBuildLoad2(backend->builder, memory_type,
            address,
                                "native_record_abi_part");
        if (loaded == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else if (part->alignment != 0) {
            LLVMSetAlignment(loaded, part->alignment);
        }
        if (status == OM_STATUS_OK
            && part->mode == OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND
            && LLVMTypeOf(loaded) != part->llvm_type) {
            if (LLVMGetTypeKind(LLVMTypeOf(loaded)) != LLVMIntegerTypeKind
                || LLVMGetTypeKind(part->llvm_type) != LLVMIntegerTypeKind) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                const unsigned source_bits =
                    LLVMGetIntTypeWidth(LLVMTypeOf(loaded));
                const unsigned target_bits = LLVMGetIntTypeWidth(
                    part->llvm_type);
                *value = source_bits < target_bits
                    ? LLVMBuildZExt(backend->builder, loaded,
                        part->llvm_type, "native_record_abi_extend")
                    : LLVMBuildTrunc(backend->builder, loaded,
                        part->llvm_type, "native_record_abi_truncate");
                if (*value == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
        } else if (status == OM_STATUS_OK) {
            if (LLVMTypeOf(loaded) != part->llvm_type) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                *value = loaded;
            }
        }
    }
    return status;
}

static Om_Status store_native_record_part(Om_Llvm_Backend_Context *backend,
                                          LLVMValueRef slot,
                                          const Om_Llvm_Native_Record_Part *part,
                                          LLVMValueRef value)
{
    if (backend == NULL || slot == NULL || part == NULL
        || part->llvm_type == NULL || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef memory_type = part->memory_type != NULL
        ? part->memory_type : part->llvm_type;
    if (memory_type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (LLVMTypeOf(value) != memory_type) {
        if (part->mode != OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND
            || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(memory_type) != LLVMIntegerTypeKind) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const unsigned source_bits = LLVMGetIntTypeWidth(LLVMTypeOf(value));
        const unsigned target_bits = LLVMGetIntTypeWidth(memory_type);
        value = source_bits > target_bits
            ? LLVMBuildTrunc(backend->builder, value, memory_type,
                             "native_record_result_truncate")
            : LLVMBuildZExt(backend->builder, value, memory_type,
                            "native_record_result_extend");
        if (value == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef address = NULL;
    Om_Status status = native_record_part_address(backend, slot, part,
                                                   &address);
    if (status == OM_STATUS_OK) {
        LLVMValueRef store = LLVMBuildStore(backend->builder, value, address);
        if (store == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else if (part->alignment != 0) {
            LLVMSetAlignment(store, part->alignment);
        }
    }
    return status;
}

Om_Status add_native_integer_extension_attribute(
    Om_Llvm_Backend_Context *backend,
    LLVMValueRef function,
    LLVMValueRef call,
    LLVMAttributeIndex index,
    const Om_Llvm_Native_Abi_Type *abi_type)
{
    if (abi_type == NULL || !abi_type->is_integer
        || abi_type->bit_width >= 32) {
        return OM_STATUS_OK;
    }
    const char *name = abi_type->is_signed ? "signext" : "zeroext";
    const unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (kind == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMAttributeRef attribute = LLVMCreateEnumAttribute(backend->context,
                                                          kind, 0);
    if (attribute == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (function != NULL) {
        LLVMAddAttributeAtIndex(function, index, attribute);
    }
    if (call != NULL) {
        LLVMAddCallSiteAttribute(call, index, attribute);
    }
    return OM_STATUS_OK;
}

static Om_Status declare_native_external(Om_Llvm_Backend_Context *backend,
                                         Om_String_View symbol,
                                         LLVMTypeRef signature,
                                         LLVMValueRef *function_out)
{
    if (backend == NULL || !om_string_view_is_valid(symbol)
        || symbol.length == 0 || symbol.length == SIZE_MAX
        || memchr(symbol.data, '\0', symbol.length) != NULL
        || signature == NULL || function_out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *function_out = NULL;
    char *symbol_name = om_alloc(symbol.length + 1);
    if (symbol_name == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(symbol_name, symbol.data, symbol.length);
    symbol_name[symbol.length] = '\0';
    LLVMValueRef function = LLVMGetNamedFunctionWithLength(backend->module,
        symbol_name, symbol.length);
    Om_Status status = OM_STATUS_OK;
    if (function != NULL
        && LLVMGlobalGetValueType(function) != signature) {
        status = OM_STATUS_INVALID_ARGUMENT;
    } else if (function == NULL) {
        function = LLVMAddFunction(backend->module, symbol_name, signature);
        if (function == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK) {
        *function_out = function;
    }
    om_free(symbol_name);
    return status;
}

static Om_Status native_release_function(
    Om_Llvm_Backend_Context *backend,
    const Om_Omi_Module *module,
    Om_String_View release_name,
    LLVMValueRef *release_function_out)
{
    if (backend == NULL || module == NULL || release_function_out == NULL
        || module->functions == NULL
        || !om_string_view_is_valid(release_name)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *release_function_out = NULL;
    const Om_Omi_Native_Function *release = NULL;
    for (size_t index = 0; index < module->function_count; ++index) {
        if (native_metadata_name_equal(module->functions[index].name,
                                       release_name)) {
            release = &module->functions[index];
            break;
        }
    }
    if (release == NULL || release->result_type != OM_NATIVE_ABI_CVOID
        || release->parameter_count != 1
        || release->parameter_start > module->parameter_count
        || release->parameter_count
            > module->parameter_count - release->parameter_start
        || module->parameters == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Parameter *parameter =
        &module->parameters[release->parameter_start];
    if (parameter->is_callback
        || (parameter->abi_type != OM_NATIVE_ABI_CPOINTER
            && parameter->abi_type != OM_NATIVE_ABI_CSTRING)
        || parameter->ownership != OM_NATIVE_OWNERSHIP_OWNED) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef signature = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), parameters, 1, 0);
    return declare_native_external(backend, release->symbol, signature,
                                   release_function_out);
}

static Om_Status native_pointer_result(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Module *module,
    const Om_Omi_Native_Function *native_function,
    const Om_Mir_Value_Id *operands,
    LLVMValueRef native_result,
    LLVMValueRef *managed_result)
{
    if (backend == NULL || function == NULL || module == NULL
        || native_function == NULL || native_result == NULL
        || managed_result == NULL
        || native_function->result_type != OM_NATIVE_ABI_CPOINTER
        || native_function->parameter_start > module->parameter_count
        || native_function->parameter_count
            > module->parameter_count - native_function->parameter_start
        || (native_function->parameter_count != 0
            && (module->parameters == NULL || operands == NULL))) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Native_Ownership ownership = native_function->result_ownership;
    if (ownership != OM_NATIVE_OWNERSHIP_BORROWED
        && ownership != OM_NATIVE_OWNERSHIP_STATIC
        && ownership != OM_NATIVE_OWNERSHIP_OWNED) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    LLVMTypeRef release_parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef release_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), release_parameters, 1, 0);
    LLVMTypeRef release_pointer_type = LLVMPointerType(release_type, 0);
    LLVMValueRef release_function = LLVMConstNull(release_pointer_type);
    if (ownership == OM_NATIVE_OWNERSHIP_OWNED) {
        Om_Status status = native_release_function(backend, module,
            native_function->release_function, &release_function);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }

    LLVMValueRef owner = LLVMConstNull(backend->byte_pointer_type);
    if (ownership == OM_NATIVE_OWNERSHIP_BORROWED) {
        size_t owner_index = SIZE_MAX;
        for (size_t index = 0; index < native_function->parameter_count;
             ++index) {
            const Om_Omi_Parameter *parameter = &module->parameters[
                native_function->parameter_start + index];
            if (native_metadata_name_equal(parameter->name,
                    native_function->result_owner_parameter)) {
                owner_index = index;
                break;
            }
        }
        if (owner_index == SIZE_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Omi_Parameter *owner_parameter = &module->parameters[
            native_function->parameter_start + owner_index];
        if (owner_parameter->is_callback
            || owner_parameter->abi_type != OM_NATIVE_ABI_CPOINTER
            || owner_parameter->ownership == OM_NATIVE_OWNERSHIP_OWNED) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        size_t source_owner_index = 0;
        for (size_t index = 0; index < owner_index; ++index) {
            if (!module->parameters[native_function->parameter_start + index]
                    .is_callback) {
                ++source_owner_index;
            }
        }
        owner = operands == NULL ? NULL
            : value_at(backend, operands[source_owner_index]);
        if (owner == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }

    LLVMValueRef context = context_for_function(backend, function);
    LLVMValueRef result_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "native_pointer_result_slot");
    if (context == NULL || result_slot == NULL) {
        return context == NULL ? OM_STATUS_INVALID_ARGUMENT
                               : OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), result_slot);
    LLVMValueRef arguments[5] = {
        context, native_result, release_function, owner, result_slot,
    };
    LLVMValueRef create_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_native_pointer_create),
        backend->runtime_native_pointer_create, arguments, 5,
        "native_pointer_create_status");
    Om_Status status = emit_runtime_status_check(backend, block_id,
                                                 create_status);
    if (status == OM_STATUS_OK) {
        *managed_result = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, result_slot,
            "native_managed_pointer_result");
        if (*managed_result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    return status;
}

static Om_Status native_cstring_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument)
{
    if (backend == NULL || function == NULL || parameter == NULL
        || value == NULL || argument == NULL
        || parameter->abi_type != OM_NATIVE_ABI_CSTRING
        || parameter->ownership != OM_NATIVE_OWNERSHIP_BORROWED) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef size_type = LLVMIntPtrTypeInContext(backend->context,
                                                     backend->target_data);
    LLVMValueRef bytes_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "native_cstring_bytes_slot");
    LLVMValueRef length_slot = entry_alloca(backend, function,
        size_type, "native_cstring_length_slot");
    if (bytes_slot == NULL || length_slot == NULL || size_type == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), bytes_slot);
    LLVMBuildStore(backend->builder, LLVMConstInt(size_type, 0, 0),
                   length_slot);
    LLVMValueRef view_arguments[4] = {
        context_for_function(backend, function),
        value,
        bytes_slot,
        length_slot,
    };
    if (view_arguments[0] == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef view_call = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_string_cstring_view),
        backend->runtime_string_cstring_view, view_arguments, 4,
        "native_cstring_view_status");
    Om_Status status = emit_runtime_status_check(backend, block_id, view_call);
    if (status != OM_STATUS_OK) {
        return status;
    }
    LLVMValueRef bytes = LLVMBuildLoad2(backend->builder,
        backend->byte_pointer_type, bytes_slot, "native_cstring_source_bytes");
    LLVMValueRef length = LLVMBuildLoad2(backend->builder, size_type,
        length_slot, "native_cstring_source_length");
    LLVMValueRef capacity = LLVMBuildAdd(backend->builder, length,
        LLVMConstInt(size_type, 1, 0), "native_cstring_capacity");
    LLVMValueRef stack_cstring = LLVMBuildArrayAlloca(backend->builder,
        backend->i8_type, capacity, "native_cstring_argument");
    if (bytes == NULL || length == NULL || capacity == NULL
        || stack_cstring == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef copy = LLVMBuildMemCpy(backend->builder,
        stack_cstring, 1, bytes, 1, length);
    LLVMValueRef terminator = LLVMBuildGEP2(backend->builder,
        backend->i8_type, stack_cstring, &length, 1,
        "native_cstring_terminator");
    if (copy == NULL || terminator == NULL
        || LLVMBuildStore(backend->builder,
            LLVMConstInt(backend->i8_type, 0, 0), terminator) == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *argument = stack_cstring;
    return OM_STATUS_OK;
}

static Om_Status native_utf8_slice_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument)
{
    if (backend == NULL || function == NULL || parameter == NULL
        || value == NULL || argument == NULL
        || parameter->abi_type != OM_NATIVE_ABI_CUTF8_SLICE
        || parameter->ownership != OM_NATIVE_OWNERSHIP_BORROWED) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned pointer_bits = LLVMPointerSize(backend->target_data)
        * CHAR_BIT;
    LLVMTypeRef size_type = LLVMIntTypeInContext(backend->context,
                                                 pointer_bits);
    LLVMTypeRef fields[2] = {backend->byte_pointer_type, size_type};
    LLVMTypeRef view_type = LLVMStructTypeInContext(backend->context,
                                                    fields, 2, 0);
    LLVMValueRef view_slot = entry_alloca(backend, function, view_type,
                                          "native_utf8_slice_argument");
    LLVMValueRef context = context_for_function(backend, function);
    if (view_slot == NULL || context == NULL) {
        return view_slot == NULL ? OM_STATUS_OUT_OF_MEMORY
                                 : OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef view_arguments[3] = {
        context, value,
        LLVMBuildBitCast(backend->builder, view_slot,
            backend->byte_pointer_type, "native_utf8_slice_view_out"),
    };
    if (view_arguments[2] == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMValueRef view_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_string_view),
        backend->runtime_string_view, view_arguments, 3,
        "native_utf8_slice_view_status");
    Om_Status status = emit_runtime_status_check(backend, block_id,
                                                 view_status);
    if (status != OM_STATUS_OK) return status;
    *argument = LLVMBuildBitCast(backend->builder, view_slot,
        backend->byte_pointer_type, "native_utf8_slice_argument_pointer");
    return *argument == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static Om_Status native_pointer_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument)
{
    if (backend == NULL || function == NULL || parameter == NULL
        || value == NULL || argument == NULL
        || parameter->abi_type != OM_NATIVE_ABI_CPOINTER
        || (parameter->ownership != OM_NATIVE_OWNERSHIP_BORROWED
            && parameter->ownership != OM_NATIVE_OWNERSHIP_STATIC
            && parameter->ownership != OM_NATIVE_OWNERSHIP_OWNED)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef raw_pointer_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "native_pointer_argument_slot");
    LLVMValueRef context = context_for_function(backend, function);
    if (raw_pointer_slot == NULL || context == NULL) {
        return raw_pointer_slot == NULL ? OM_STATUS_OUT_OF_MEMORY
                                        : OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), raw_pointer_slot);
    LLVMValueRef arguments[3] = {context, value, raw_pointer_slot};
    LLVMValueRef access_function =
        parameter->ownership == OM_NATIVE_OWNERSHIP_OWNED
            ? backend->runtime_native_pointer_take
            : backend->runtime_native_pointer_borrow;
    LLVMValueRef access_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(access_function), access_function,
        arguments, 3, "native_pointer_access_status");
    Om_Status status = emit_runtime_status_check(backend, block_id,
                                                 access_status);
    if (status == OM_STATUS_OK) {
        *argument = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, raw_pointer_slot,
            "native_pointer_argument");
        if (*argument == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    return status;
}

static Om_Status native_scalar_call_argument(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    const Om_Llvm_Native_Abi_Type *abi_type,
    LLVMValueRef value,
    LLVMValueRef *argument)
{
    if (backend == NULL || parameter == NULL || abi_type == NULL
        || value == NULL || argument == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (abi_type->is_pointer) {
        if (parameter->abi_type == OM_NATIVE_ABI_CSTRING) {
            return native_cstring_call_argument(backend, function, block_id,
                                                parameter, value, argument);
        }
        if (parameter->abi_type == OM_NATIVE_ABI_CUTF8_SLICE) {
            return native_utf8_slice_call_argument(backend, function, block_id,
                                                   parameter, value, argument);
        }
        if (parameter->abi_type == OM_NATIVE_ABI_CPOINTER) {
            return native_pointer_call_argument(backend, function, block_id,
                                                parameter, value, argument);
        }
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (abi_type->is_float && abi_type->bit_width == 32) {
        *argument = LLVMBuildFPTrunc(backend->builder, value,
            abi_type->llvm_type, "native_float_argument");
    } else if (abi_type->is_integer && !abi_type->is_boolean
               && abi_type->bit_width < 64) {
        *argument = LLVMBuildTrunc(backend->builder, value,
            abi_type->llvm_type, "native_integer_argument");
    } else {
        *argument = value;
    }
    return *argument == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static Om_Status emit_native_record_call(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Mir_Instruction *instruction,
    const Om_Mir_Value_Id *operands,
    LLVMValueRef *result,
    const Om_Symbol *symbol,
    const Om_Omi_Native_Function *native_function)
{
    const Om_Omi_Module *module = symbol->native_module;
    if (module == NULL || native_function == NULL || result == NULL
        || native_function->parameter_start > module->parameter_count
        || native_function->parameter_count
            > module->parameter_count - native_function->parameter_start
        || (native_function->parameter_count != 0
            && module->parameters == NULL)
        || native_function->parameter_count != instruction->operand_count
        || (native_function->parameter_count != 0 && operands == NULL)
        || native_function->parameter_count > UINT_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        if (module->parameters[native_function->parameter_start + index]
                .is_callback) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }

    const bool result_is_record =
        native_function->result_type == OM_NATIVE_ABI_RECORD;
    Om_Llvm_Native_Record_Abi result_record = {0};
    Om_Llvm_Native_Abi_Type result_scalar = {0};
    Om_Status status = result_is_record
        ? native_record_abi(backend, module,
              native_function->result_record_index, &result_record)
        : native_abi_type(backend, native_function->result_type, true,
                          &result_scalar);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Llvm_Native_Call_Parameter *parameters =
        native_function->parameter_count == 0 ? NULL
        : om_alloc_array(native_function->parameter_count,
                          sizeof(*parameters));
    if (native_function->parameter_count != 0 && parameters == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t abi_parameter_count = result_is_record && result_record.indirect
        ? 1 : 0;
    bool needs_stack_restore =
        native_function->result_type == OM_NATIVE_ABI_CSTRING;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        plan->is_record = parameter->abi_type == OM_NATIVE_ABI_RECORD;
        if (plan->is_record) {
            status = native_record_abi(backend, module,
                parameter->record_index, &plan->record);
            plan->abi_count = plan->record.indirect
                ? 1 : plan->record.part_count;
        } else {
            status = native_abi_type(backend, parameter->abi_type, false,
                                     &plan->scalar);
            plan->abi_count = 1;
            needs_stack_restore = needs_stack_restore
                || parameter->abi_type == OM_NATIVE_ABI_CSTRING;
        }
        plan->abi_start = abi_parameter_count;
        if (status == OM_STATUS_OK
            && !om_checked_size_add(abi_parameter_count, plan->abi_count,
                                    &abi_parameter_count)) {
            status = OM_STATUS_OVERFLOW;
        }
    }
    if (status == OM_STATUS_OK && abi_parameter_count > UINT_MAX) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    LLVMTypeRef return_type = NULL;
    if (status == OM_STATUS_OK) {
        if (result_is_record && result_record.indirect) {
            return_type = LLVMVoidTypeInContext(backend->context);
        } else if (result_is_record) {
            status = native_record_return_type(backend, &result_record,
                                               &return_type);
        } else {
            return_type = result_scalar.llvm_type;
        }
    }
    LLVMTypeRef *parameter_types = abi_parameter_count == 0 ? NULL
        : om_alloc_array(abi_parameter_count, sizeof(*parameter_types));
    LLVMValueRef *arguments = abi_parameter_count == 0 ? NULL
        : om_alloc_array(abi_parameter_count, sizeof(*arguments));
    if (status == OM_STATUS_OK && abi_parameter_count != 0
        && (parameter_types == NULL || arguments == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    size_t next_type = 0;
    if (status == OM_STATUS_OK && result_is_record && result_record.indirect) {
        parameter_types[next_type++] = backend->byte_pointer_type;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        if (plan->is_record && plan->record.indirect) {
            parameter_types[next_type++] = backend->byte_pointer_type;
        } else if (plan->is_record) {
            for (size_t part = 0; part < plan->record.part_count; ++part) {
                parameter_types[next_type++] =
                    plan->record.parts[part].llvm_type;
            }
        } else {
            parameter_types[next_type++] = plan->scalar.llvm_type;
        }
    }
    if (status == OM_STATUS_OK && next_type != abi_parameter_count) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef signature = status == OM_STATUS_OK
        ? LLVMFunctionType(return_type, parameter_types,
            (unsigned)abi_parameter_count, 0) : NULL;
    if (status == OM_STATUS_OK && signature == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef native_function_value = NULL;
    if (status == OM_STATUS_OK) {
        status = declare_native_external(backend, native_function->symbol,
            signature, &native_function_value);
    }
    if (status == OM_STATUS_OK && !result_is_record) {
        status = add_native_integer_extension_attribute(backend,
            native_function_value, NULL, LLVMAttributeReturnIndex,
            &result_scalar);
    }
    LLVMValueRef result_slot = NULL;
    if (status == OM_STATUS_OK && result_is_record && result_record.indirect) {
        result_slot = entry_alloca(backend, function, result_record.llvm_type,
                                   "native_record_sret_slot");
        if (result_slot == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            arguments[0] = result_slot;
            status = add_native_type_attribute(backend,
                native_function_value, NULL, (LLVMAttributeIndex)1,
                "sret", result_record.llvm_type);
            if (status == OM_STATUS_OK) {
                status = add_native_enum_attribute(backend,
                    native_function_value, NULL, (LLVMAttributeIndex)1,
                    "align", result_record.alignment);
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        const LLVMAttributeIndex first_index = (LLVMAttributeIndex)(
            plan->abi_start + 1);
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (plan->is_record && plan->record.indirect) {
            if (plan->record.byval) {
                status = add_native_type_attribute(backend,
                    native_function_value, NULL, first_index, "byval",
                    plan->record.llvm_type);
            }
            if (status == OM_STATUS_OK) {
                status = add_native_enum_attribute(backend,
                    native_function_value, NULL, first_index, "align",
                    plan->record.alignment);
            }
        } else if (!plan->is_record) {
            status = add_native_integer_extension_attribute(backend,
                native_function_value, NULL, first_index, &plan->scalar);
        } else {
            for (size_t part = 0; status == OM_STATUS_OK
                 && part < plan->record.part_count; ++part) {
                const Om_Llvm_Native_Record_Part *record_part =
                    &plan->record.parts[part];
                if (record_part->abi_alignment != 0) {
                    status = add_native_enum_attribute(backend,
                        native_function_value, NULL,
                        (LLVMAttributeIndex)(plan->abi_start + part + 1),
                        "alignstack", record_part->abi_alignment);
                }
            }
        }
        (void)parameter;
    }
    LLVMValueRef saved_stack = NULL;
    if (status == OM_STATUS_OK && needs_stack_restore) {
        static const char stack_save_name[] = "llvm.stacksave";
        const unsigned stack_save_id = LLVMLookupIntrinsicID(
            stack_save_name, sizeof(stack_save_name) - 1);
        LLVMTypeRef stack_save_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_save_function = stack_save_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_save_id,
                stack_save_overload_types, 1);
        if (stack_save_function == NULL) {
            status = OM_STATUS_NOT_FOUND;
        } else {
            saved_stack = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(stack_save_function),
                stack_save_function, NULL, 0, "native_stack_save");
            if (saved_stack == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        LLVMValueRef value = value_at(backend, operands[index]);
        if (value == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const size_t argument_index = plan->abi_start;
        if (plan->is_record) {
            LLVMValueRef slot = entry_alloca(backend, function,
                plan->record.llvm_type, "native_record_argument_slot");
            if (slot == NULL || LLVMBuildStore(backend->builder, value,
                                               slot) == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            if (plan->record.indirect) {
                arguments[argument_index] = slot;
            } else {
                for (size_t part = 0; status == OM_STATUS_OK
                     && part < plan->record.part_count; ++part) {
                    status = load_native_record_part(backend, slot,
                        &plan->record.parts[part],
                        &arguments[argument_index + part]);
                }
            }
        } else {
            const Om_Omi_Parameter *parameter = &module->parameters[
                native_function->parameter_start + index];
            status = native_scalar_call_argument(backend, function, block_id,
                parameter, &plan->scalar, value,
                &arguments[argument_index]);
        }
    }
    LLVMValueRef call = NULL;
    if (status == OM_STATUS_OK) {
        call = LLVMBuildCall2(backend->builder, signature,
            native_function_value, arguments, (unsigned)abi_parameter_count,
            native_function->result_type == OM_NATIVE_ABI_CVOID
                || (result_is_record && result_record.indirect)
                ? "" : "native_call_result");
        if (call == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK && !result_is_record) {
        status = add_native_integer_extension_attribute(backend, NULL, call,
            LLVMAttributeReturnIndex, &result_scalar);
    }
    if (status == OM_STATUS_OK && result_is_record && result_record.indirect) {
        status = add_native_type_attribute(backend, NULL, call,
            (LLVMAttributeIndex)1, "sret", result_record.llvm_type);
        if (status == OM_STATUS_OK) {
            status = add_native_enum_attribute(backend, NULL, call,
                (LLVMAttributeIndex)1, "align", result_record.alignment);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        const LLVMAttributeIndex first_index = (LLVMAttributeIndex)(
            plan->abi_start + 1);
        if (plan->is_record && plan->record.indirect) {
            if (plan->record.byval) {
                status = add_native_type_attribute(backend, NULL, call,
                    first_index, "byval", plan->record.llvm_type);
            }
            if (status == OM_STATUS_OK) {
                status = add_native_enum_attribute(backend, NULL, call,
                    first_index, "align", plan->record.alignment);
            }
        } else if (!plan->is_record) {
            status = add_native_integer_extension_attribute(backend, NULL,
                call, first_index, &plan->scalar);
        } else {
            for (size_t part = 0; status == OM_STATUS_OK
                 && part < plan->record.part_count; ++part) {
                const Om_Llvm_Native_Record_Part *record_part =
                    &plan->record.parts[part];
                if (record_part->abi_alignment != 0) {
                    status = add_native_enum_attribute(backend, NULL, call,
                        (LLVMAttributeIndex)(plan->abi_start + part + 1),
                        "alignstack", record_part->abi_alignment);
                }
            }
        }
    }

    if (status == OM_STATUS_OK && result_is_record) {
        if (result_record.indirect) {
            *result = LLVMBuildLoad2(backend->builder, result_record.llvm_type,
                result_slot, "native_record_sret_result");
        } else if (result_record.result_is_record) {
            *result = call;
        } else {
            result_slot = entry_alloca(backend, function,
                result_record.llvm_type, "native_record_result_slot");
            if (result_slot == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const Om_Llvm_Native_Record_Part *result_parts =
                    result_record.result_part_count != 0
                        ? result_record.result_parts : result_record.parts;
                const size_t result_part_count =
                    result_record.result_part_count != 0
                        ? result_record.result_part_count
                        : result_record.part_count;
                if (result_part_count == 1) {
                    status = store_native_record_part(backend, result_slot,
                        &result_parts[0], call);
                } else {
                    for (size_t part = 0; status == OM_STATUS_OK
                         && part < result_part_count; ++part) {
                        LLVMValueRef component = LLVMBuildExtractValue(
                            backend->builder, call, (unsigned)part,
                            "native_record_result_part");
                        if (component == NULL) {
                            status = OM_STATUS_OUT_OF_MEMORY;
                        } else {
                            status = store_native_record_part(backend,
                                result_slot, &result_parts[part], component);
                        }
                    }
                }
                if (status == OM_STATUS_OK) {
                    *result = LLVMBuildLoad2(backend->builder,
                        result_record.llvm_type, result_slot,
                        "native_record_result");
                }
            }
        }
        if (status == OM_STATUS_OK && *result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    } else if (status == OM_STATUS_OK) {
        if (native_function->result_type == OM_NATIVE_ABI_CVOID) {
            *result = LLVMConstNull(backend->i8_type);
        } else if (native_function->result_type == OM_NATIVE_ABI_CSTRING) {
            if (native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_BORROWED
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_STATIC
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_OWNED) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                LLVMValueRef managed_result_slot = entry_alloca(backend,
                    function, backend->byte_pointer_type,
                    "native_string_result_slot");
                if (managed_result_slot == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                } else {
                    LLVMBuildStore(backend->builder,
                        LLVMConstNull(backend->byte_pointer_type),
                        managed_result_slot);
                    const bool owned_result = native_function->result_ownership
                        == OM_NATIVE_OWNERSHIP_OWNED;
                    LLVMValueRef string_arguments[4] = {
                        context_for_function(backend, function),
                        call,
                        managed_result_slot,
                        NULL,
                    };
                    LLVMValueRef string_function = NULL;
                    unsigned string_argument_count = 3;
                    if (string_arguments[0] == NULL) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                    } else if (owned_result) {
                        LLVMValueRef release_function_value = NULL;
                        status = native_release_function(backend, module,
                            native_function->release_function,
                            &release_function_value);
                        if (status == OM_STATUS_OK) {
                            string_arguments[2] = release_function_value;
                            string_function =
                                backend->runtime_string_from_owned_cstring;
                            string_argument_count = 4;
                        }
                    } else {
                        string_function =
                            backend->runtime_string_from_cstring;
                    }
                    if (status == OM_STATUS_OK) {
                        LLVMValueRef string_status = LLVMBuildCall2(
                            backend->builder,
                            LLVMGlobalGetValueType(string_function),
                            string_function, string_arguments,
                            string_argument_count,
                            "native_string_conversion_status");
                        status = emit_runtime_status_check(backend, block_id,
                                                           string_status);
                    }
                    if (status == OM_STATUS_OK) {
                        *result = LLVMBuildLoad2(backend->builder,
                            backend->byte_pointer_type, managed_result_slot,
                            "native_managed_string_result");
                    }
                }
            }
        } else if (native_function->result_type == OM_NATIVE_ABI_CPOINTER) {
            status = native_pointer_result(backend, function, block_id,
                module, native_function, operands, call, result);
        } else if (native_function->result_type
                   == OM_NATIVE_ABI_CUTF8_SLICE) {
            LLVMValueRef managed_result_slot = entry_alloca(backend,
                function, backend->byte_pointer_type,
                "native_utf8_string_result_slot");
            LLVMValueRef release_function_value = NULL;
            if (managed_result_slot == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                status = native_release_function(backend, module,
                    native_function->release_function,
                    &release_function_value);
            }
            if (status == OM_STATUS_OK) {
                LLVMBuildStore(backend->builder,
                    LLVMConstNull(backend->byte_pointer_type),
                    managed_result_slot);
                LLVMValueRef string_arguments[4] = {
                    context_for_function(backend, function), call,
                    release_function_value, managed_result_slot,
                };
                if (string_arguments[0] == NULL) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                } else {
                    LLVMValueRef string_status = LLVMBuildCall2(
                        backend->builder,
                        LLVMGlobalGetValueType(
                            backend->runtime_string_from_owned_utf8_slice),
                        backend->runtime_string_from_owned_utf8_slice,
                        string_arguments, 4,
                        "native_utf8_string_conversion_status");
                    status = emit_runtime_status_check(backend, block_id,
                                                       string_status);
                }
            }
            if (status == OM_STATUS_OK) {
                *result = LLVMBuildLoad2(backend->builder,
                    backend->byte_pointer_type, managed_result_slot,
                    "native_managed_utf8_string_result");
            }
        } else if (result_scalar.is_float && result_scalar.bit_width == 32) {
            *result = LLVMBuildFPExt(backend->builder, call,
                backend->double_type, "native_float_result");
        } else if (result_scalar.is_integer && !result_scalar.is_boolean
                   && result_scalar.bit_width < 64) {
            *result = result_scalar.is_signed
                ? LLVMBuildSExt(backend->builder, call, backend->i64_type,
                    "native_signed_result")
                : LLVMBuildZExt(backend->builder, call, backend->i64_type,
                    "native_unsigned_result");
        } else {
            *result = call;
        }
        if (status == OM_STATUS_OK && *result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK && needs_stack_restore) {
        static const char stack_restore_name[] = "llvm.stackrestore";
        const unsigned stack_restore_id = LLVMLookupIntrinsicID(
            stack_restore_name, sizeof(stack_restore_name) - 1);
        LLVMTypeRef stack_restore_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_restore_function = stack_restore_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_restore_id,
                stack_restore_overload_types, 1);
        if (stack_restore_function == NULL) {
            status = OM_STATUS_NOT_FOUND;
        } else {
            LLVMValueRef restore_arguments[1] = {saved_stack};
            if (LLVMBuildCall2(backend->builder,
                    LLVMGlobalGetValueType(stack_restore_function),
                    stack_restore_function, restore_arguments, 1, "") == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    om_free(parameters);
    om_free(parameter_types);
    om_free(arguments);
    return status;
}

Om_Status emit_native_call(Om_Llvm_Backend_Context *backend,
                                  const Om_Mir_Function *function,
                                  Om_Mir_Block_Id block_id,
                                  const Om_Mir_Instruction *instruction,
                                  const Om_Mir_Value_Id *operands,
                                  LLVMValueRef *result)
{
    if (backend == NULL || instruction == NULL || result == NULL
        || function == NULL
        || backend->project->resolution == NULL
        || instruction->symbol_id.value
            >= backend->project->resolution->symbol_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        backend->project->resolution, instruction->symbol_id);
    if (symbol == NULL || symbol->native_module == NULL
        || symbol->native_function_index
            >= symbol->native_module->function_count
        || symbol->native_module->functions == NULL) {
        return OM_STATUS_NOT_FOUND;
    }
    const Om_Omi_Module *module = symbol->native_module;
    const Om_Omi_Native_Function *native_function =
        &module->functions[symbol->native_function_index];
    if (native_function->parameter_start > module->parameter_count
        || native_function->parameter_count
            > module->parameter_count - native_function->parameter_start
        || (native_function->parameter_count != 0
            && module->parameters == NULL)
        || native_function->callback_binding_start
            > module->callback_binding_count
        || native_function->callback_binding_count
            > module->callback_binding_count
                - native_function->callback_binding_start
        || (native_function->callback_binding_count != 0
            && module->callback_bindings == NULL)
        || native_function->parameter_count > UINT_MAX
        || (instruction->operand_count != 0 && operands == NULL)
        || native_function->symbol.data == NULL
        || native_function->symbol.length == 0
        || memchr(native_function->symbol.data, '\0',
                  native_function->symbol.length) != NULL
        || native_function->symbol.length == SIZE_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    size_t source_parameter_count = 0;
    size_t declared_callback_count = 0;
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback) ++declared_callback_count;
        else ++source_parameter_count;
    }
    if (declared_callback_count != native_function->callback_binding_count
        || source_parameter_count != instruction->operand_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    bool has_native_record = native_function->result_type
        == OM_NATIVE_ABI_RECORD;
    for (size_t index = 0; !has_native_record
         && index < native_function->parameter_count; ++index) {
        has_native_record = module->parameters[
            native_function->parameter_start + index].abi_type
            == OM_NATIVE_ABI_RECORD;
    }
    if (has_native_record) {
        if (declared_callback_count != 0) return OM_STATUS_INVALID_ARGUMENT;
        return emit_native_record_call(backend, function, block_id,
            instruction, operands, result, symbol, native_function);
    }

    Om_Llvm_Native_Callback_Call callback_call = {0};
    Om_Status status = native_callback_call_prepare(backend, module,
        native_function, &callback_call);
    if (status != OM_STATUS_OK) return status;

    bool needs_stack_restore =
        native_function->result_type == OM_NATIVE_ABI_CSTRING;
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        if (module->parameters[native_function->parameter_start + index]
                .abi_type == OM_NATIVE_ABI_CSTRING) {
            needs_stack_restore = true;
            break;
        }
    }
    LLVMValueRef saved_stack = NULL;
    if (needs_stack_restore) {
        static const char stack_save_name[] = "llvm.stacksave";
        const unsigned stack_save_id = LLVMLookupIntrinsicID(
            stack_save_name, sizeof(stack_save_name) - 1);
        LLVMTypeRef stack_save_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_save_function = stack_save_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_save_id,
                stack_save_overload_types, 1);
        if (stack_save_function == NULL) {
            native_callback_call_destroy(&callback_call);
            return OM_STATUS_NOT_FOUND;
        }
        saved_stack = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(stack_save_function),
            stack_save_function, NULL, 0, "native_stack_save");
        if (saved_stack == NULL) {
            native_callback_call_destroy(&callback_call);
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }

    Om_Llvm_Native_Abi_Type return_abi = {0};
    status = native_abi_type(backend, native_function->result_type,
                             true, &return_abi);
    Om_Llvm_Native_Abi_Type *parameter_abi = native_function->parameter_count
        == 0 ? NULL : om_alloc_array(native_function->parameter_count,
                                     sizeof(*parameter_abi));
    LLVMTypeRef *parameter_types = native_function->parameter_count == 0
        ? NULL : om_alloc_array(native_function->parameter_count,
                                sizeof(*parameter_types));
    LLVMValueRef *arguments = native_function->parameter_count == 0
        ? NULL : om_alloc_array(native_function->parameter_count,
                                sizeof(*arguments));
    if (status == OM_STATUS_OK && native_function->parameter_count != 0
        && (parameter_abi == NULL || parameter_types == NULL
            || arguments == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback) {
            const size_t binding_index =
                callback_call.binding_for_parameter == NULL ? SIZE_MAX
                    : callback_call.binding_for_parameter[index];
            if (binding_index >= callback_call.callback_count
                || callback_call.thunk_sets[binding_index] == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                parameter_abi[index] = (Om_Llvm_Native_Abi_Type){
                    .llvm_type = callback_call.thunk_sets[binding_index]
                        ->function_pointer_type,
                    .bit_width = LLVMPointerSize(backend->target_data)
                        * CHAR_BIT,
                    .is_pointer = true,
                };
                parameter_types[index] = parameter_abi[index].llvm_type;
            }
            continue;
        }
        status = native_abi_type(backend, parameter->abi_type, false,
                                 &parameter_abi[index]);
        if (status == OM_STATUS_OK) {
            parameter_types[index] = parameter_abi[index].llvm_type;
        }
    }
    LLVMTypeRef signature = NULL;
    if (status == OM_STATUS_OK) {
        signature = LLVMFunctionType(return_abi.llvm_type, parameter_types,
            (unsigned)native_function->parameter_count, 0);
        if (signature == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef native_function_value = NULL;
    if (status == OM_STATUS_OK) {
        status = declare_native_external(backend, native_function->symbol,
            signature, &native_function_value);
    }
    if (status == OM_STATUS_OK) {
        status = add_native_integer_extension_attribute(backend,
            native_function_value, NULL, LLVMAttributeReturnIndex, &return_abi);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        status = add_native_integer_extension_attribute(backend,
            native_function_value, NULL, (LLVMAttributeIndex)(index + 1),
            &parameter_abi[index]);
    }
    size_t source_index = 0;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback) continue;
        LLVMValueRef argument = value_at(backend, operands[source_index]);
        if (argument == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Llvm_Native_Abi_Type *abi_type = &parameter_abi[index];
        status = native_scalar_call_argument(backend, function, block_id,
            parameter, abi_type, argument, &arguments[index]);
        ++source_index;
    }
    LLVMValueRef registered_callback_slot = NULL;
    LLVMTypeRef registered_callback_array_type = NULL;
    if (status == OM_STATUS_OK && callback_call.callback_count != 0) {
        status = emit_native_callback_registration(backend, function, block_id,
            instruction, callback_call.callback_count,
            callback_call.thunk_sets, callback_call.message_descriptors,
            &registered_callback_slot, &registered_callback_array_type);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (!parameter->is_callback) continue;
        const size_t binding_index =
            callback_call.binding_for_parameter[index];
        LLVMValueRef indices[2] = {
            LLVMConstInt(backend->i32_type, 0, 0),
            LLVMConstInt(backend->i32_type, binding_index, 0),
        };
        LLVMValueRef slot_address = LLVMBuildGEP2(backend->builder,
            registered_callback_array_type, registered_callback_slot,
            indices, 2, "native_callback_slot_address");
        LLVMValueRef selected = slot_address == NULL ? NULL
            : LLVMBuildLoad2(backend->builder,
                backend->native_callback_function_pointer_type,
                slot_address, "native_callback_selected");
        if (selected == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            arguments[index] = LLVMBuildBitCast(backend->builder, selected,
                parameter_abi[index].llvm_type, "native_callback_argument");
            if (arguments[index] == NULL) status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef call = NULL;
    if (status == OM_STATUS_OK) {
        call = LLVMBuildCall2(backend->builder, signature,
            native_function_value, arguments,
            (unsigned)native_function->parameter_count,
            native_function->result_type == OM_NATIVE_ABI_CVOID
                ? "" : "native_call_result");
        if (call == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef callback_unregister_status = NULL;
    if (status == OM_STATUS_OK && callback_call.callback_count != 0) {
        LLVMValueRef unregister_arguments[3] = {
            context_for_function(backend, function),
            LLVMConstInt(backend->i64_type, callback_call.callback_count, 0),
            LLVMBuildBitCast(backend->builder, registered_callback_slot,
                backend->byte_pointer_type,
                "native_callback_unregister_pointer"),
        };
        if (unregister_arguments[0] == NULL || unregister_arguments[2] == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            callback_unregister_status = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(
                    backend->runtime_native_callback_unregister_many),
                backend->runtime_native_callback_unregister_many,
                unregister_arguments, 3,
                "native_callback_unregister_status");
            if (callback_unregister_status == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = add_native_integer_extension_attribute(backend, NULL, call,
            LLVMAttributeReturnIndex, &return_abi);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        status = add_native_integer_extension_attribute(backend, NULL, call,
            (LLVMAttributeIndex)(index + 1), &parameter_abi[index]);
    }
    if (status == OM_STATUS_OK) {
        if (native_function->result_type == OM_NATIVE_ABI_CVOID) {
            *result = LLVMConstNull(backend->i8_type);
        } else if (return_abi.is_pointer
                   && native_function->result_type == OM_NATIVE_ABI_CSTRING) {
            if (native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_BORROWED
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_STATIC
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_OWNED) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto native_call_cleanup;
            }
            LLVMValueRef managed_result_slot = entry_alloca(backend, function,
                backend->byte_pointer_type,
                "native_string_result_slot");
            if (managed_result_slot == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                goto native_call_cleanup;
            }
            LLVMBuildStore(backend->builder,
                LLVMConstNull(backend->byte_pointer_type),
                managed_result_slot);
            const bool owned_result = native_function->result_ownership
                == OM_NATIVE_OWNERSHIP_OWNED;
            LLVMValueRef string_arguments[4] = {
                context_for_function(backend, function),
                call,
                managed_result_slot,
                NULL,
            };
            LLVMValueRef string_function = NULL;
            unsigned string_argument_count = 3;
            if (string_arguments[0] == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto native_call_cleanup;
            }
            if (owned_result) {
                LLVMValueRef release_function_value = NULL;
                status = native_release_function(backend, module,
                    native_function->release_function,
                    &release_function_value);
                if (status != OM_STATUS_OK) {
                    goto native_call_cleanup;
                }
                string_arguments[2] = release_function_value;
                string_arguments[3] = managed_result_slot;
                string_function = backend->runtime_string_from_owned_cstring;
                string_argument_count = 4;
            } else {
                string_function = backend->runtime_string_from_cstring;
            }
            LLVMValueRef string_status = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(string_function), string_function,
                string_arguments, string_argument_count,
                "native_string_conversion_status");
            status = emit_runtime_status_check(backend, block_id,
                                               string_status);
            if (status != OM_STATUS_OK) {
                goto native_call_cleanup;
            }
            *result = LLVMBuildLoad2(backend->builder,
                backend->byte_pointer_type, managed_result_slot,
                "native_managed_string_result");
        } else if (return_abi.is_pointer
                   && native_function->result_type
                       == OM_NATIVE_ABI_CUTF8_SLICE) {
            LLVMValueRef managed_result_slot = entry_alloca(backend, function,
                backend->byte_pointer_type,
                "native_utf8_string_result_slot");
            if (managed_result_slot == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                goto native_call_cleanup;
            }
            LLVMBuildStore(backend->builder,
                LLVMConstNull(backend->byte_pointer_type),
                managed_result_slot);
            LLVMValueRef release_function_value = NULL;
            status = native_release_function(backend, module,
                native_function->release_function, &release_function_value);
            if (status != OM_STATUS_OK) goto native_call_cleanup;
            LLVMValueRef string_arguments[4] = {
                context_for_function(backend, function), call,
                release_function_value, managed_result_slot,
            };
            if (string_arguments[0] == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto native_call_cleanup;
            }
            LLVMValueRef string_status = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(
                    backend->runtime_string_from_owned_utf8_slice),
                backend->runtime_string_from_owned_utf8_slice,
                string_arguments, 4,
                "native_utf8_string_conversion_status");
            status = emit_runtime_status_check(backend, block_id,
                                               string_status);
            if (status != OM_STATUS_OK) goto native_call_cleanup;
            *result = LLVMBuildLoad2(backend->builder,
                backend->byte_pointer_type, managed_result_slot,
                "native_managed_utf8_string_result");
        } else if (return_abi.is_pointer
                   && native_function->result_type == OM_NATIVE_ABI_CPOINTER) {
            status = native_pointer_result(backend, function, block_id,
                module, native_function, operands, call, result);
            if (status != OM_STATUS_OK) {
                goto native_call_cleanup;
            }
        } else if (return_abi.is_float && return_abi.bit_width == 32) {
            *result = LLVMBuildFPExt(backend->builder, call,
                backend->double_type, "native_float_result");
        } else if (return_abi.is_integer && !return_abi.is_boolean
                   && return_abi.bit_width < 64) {
            *result = return_abi.is_signed
                ? LLVMBuildSExt(backend->builder, call, backend->i64_type,
                    "native_signed_result")
                : LLVMBuildZExt(backend->builder, call, backend->i64_type,
                    "native_unsigned_result");
        } else {
            *result = call;
        }
        if (*result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK && needs_stack_restore) {
        static const char stack_restore_name[] = "llvm.stackrestore";
        const unsigned stack_restore_id = LLVMLookupIntrinsicID(
            stack_restore_name, sizeof(stack_restore_name) - 1);
        LLVMTypeRef stack_restore_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_restore_function = stack_restore_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_restore_id,
                stack_restore_overload_types, 1);
        if (stack_restore_function == NULL) {
            status = OM_STATUS_NOT_FOUND;
        } else {
            LLVMValueRef restore_arguments[1] = {saved_stack};
            if (LLVMBuildCall2(backend->builder,
                    LLVMGlobalGetValueType(stack_restore_function),
                    stack_restore_function, restore_arguments, 1, "") == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    if (status == OM_STATUS_OK && callback_unregister_status != NULL) {
        status = emit_runtime_status_check(backend, block_id,
                                           callback_unregister_status);
    }
native_call_cleanup:
    native_callback_call_destroy(&callback_call);
    om_free(parameter_abi);
    om_free(parameter_types);
    om_free(arguments);
    return status;
}
