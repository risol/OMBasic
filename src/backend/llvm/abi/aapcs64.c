#include "native_abi.h"

#include <limits.h>
#include <string.h>

static unsigned part_alignment(uint64_t record_alignment, uint64_t offset)
{
    const uint64_t offset_alignment =
        offset == 0 ? record_alignment : offset & (~offset + 1);
    const uint64_t alignment = record_alignment < offset_alignment
                                   ? record_alignment
                                   : offset_alignment;
    return alignment > UINT_MAX ? UINT_MAX : (unsigned)alignment;
}

Om_Status om_llvm_native_record_classify_aapcs64_common(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, bool hfa_alignstack,
    Om_Llvm_Native_Record_Abi *abi)
{
    if (context == NULL || record_type == NULL || fields == NULL
        || field_count == 0 || abi == NULL || size == 0 || alignment == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    (void)memset(abi, 0, sizeof(*abi));
    abi->llvm_type = record_type;
    abi->size = size;
    abi->alignment = alignment;

    bool is_hfa = field_count <= 4;
    LLVMTypeRef hfa_element_type = NULL;
    unsigned hfa_element_bits = 0;
    uint64_t next_hfa_offset = 0;
    for (size_t index = 0; index < field_count; ++index) {
        const Om_Llvm_Native_Record_Field_Layout *field = &fields[index];
        if (field->llvm_type == NULL || field->size == 0
            || field->alignment == 0 || field->offset % field->alignment != 0
            || field->size > UINT64_MAX - field->offset
            || field->offset + field->size > size) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (!field->is_float || (field->bit_width != 32
                                 && field->bit_width != 64)
            || field->size != field->bit_width / CHAR_BIT
            || field->offset != next_hfa_offset
            || (hfa_element_type != NULL
                && (field->llvm_type != hfa_element_type
                    || field->bit_width != hfa_element_bits))) {
            is_hfa = false;
        }
        if (index == 0) {
            hfa_element_type = field->llvm_type;
            hfa_element_bits = field->bit_width;
        }
        next_hfa_offset = field->offset + field->size;
    }
    if (next_hfa_offset != size) {
        is_hfa = false;
    }

    if (is_hfa) {
        LLVMTypeRef array_type = LLVMArrayType(hfa_element_type,
                                               (unsigned)field_count);
        if (array_type == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        abi->parts[0] = (Om_Llvm_Native_Record_Part){
            .llvm_type = array_type,
            .memory_type = array_type,
            .offset = 0,
            .memory_size = size,
            .alignment = (unsigned)(alignment > UINT_MAX
                                        ? UINT_MAX : alignment),
            .abi_alignment = hfa_alignstack ? 8U : 0U,
            .mode = OM_LLVM_NATIVE_RECORD_PART_DIRECT,
        };
        abi->part_count = 1;
        abi->result_type = record_type;
        abi->result_is_record = true;
        return OM_STATUS_OK;
    }

    if (size > 16) {
        abi->indirect = true;
        abi->byval = true;
        return OM_STATUS_OK;
    }

    if (size <= 8) {
        if (size > UINT_MAX / CHAR_BIT) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        LLVMTypeRef memory_type = LLVMIntTypeInContext(
            context, (unsigned)(size * CHAR_BIT));
        LLVMTypeRef argument_type = LLVMIntTypeInContext(context, 64);
        if (memory_type == NULL || argument_type == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        abi->parts[0] = (Om_Llvm_Native_Record_Part){
            .llvm_type = argument_type,
            .memory_type = memory_type,
            .offset = 0,
            .memory_size = size,
            .bit_width = 64,
            .alignment = (unsigned)(alignment > UINT_MAX
                                        ? UINT_MAX : alignment),
            .mode = OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND,
            .is_integer = true,
        };
        abi->part_count = 1;

        abi->result_type = memory_type;
        abi->result_parts[0] = (Om_Llvm_Native_Record_Part){
            .llvm_type = memory_type,
            .memory_type = memory_type,
            .offset = 0,
            .memory_size = size,
            .bit_width = (unsigned)(size * CHAR_BIT),
            .alignment = (unsigned)(alignment > UINT_MAX
                                        ? UINT_MAX : alignment),
            .mode = OM_LLVM_NATIVE_RECORD_PART_DIRECT,
            .is_integer = true,
        };
        abi->result_part_count = 1;
        return OM_STATUS_OK;
    }

    LLVMTypeRef i64_type = LLVMIntTypeInContext(context, 64);
    LLVMTypeRef argument_array_type = LLVMArrayType(i64_type, 2);
    LLVMTypeRef result_array_type = LLVMArrayType(i64_type, 2);
    if (i64_type == NULL || argument_array_type == NULL
        || result_array_type == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    abi->parts[0] = (Om_Llvm_Native_Record_Part){
        .llvm_type = argument_array_type,
        .offset = 0,
        .memory_size = size,
        .alignment = (unsigned)(alignment > UINT_MAX
                                    ? UINT_MAX : alignment),
        .mode = OM_LLVM_NATIVE_RECORD_PART_INTEGER_ARRAY,
        .is_integer = true,
    };
    abi->part_count = 1;

    abi->result_type = result_array_type;
    const uint64_t second_size = size - 8;
    const uint64_t sizes[2] = {8, second_size};
    for (size_t index = 0; index < 2; ++index) {
        const unsigned memory_bits = (unsigned)(sizes[index] * CHAR_BIT);
        LLVMTypeRef memory_type = LLVMIntTypeInContext(context, memory_bits);
        if (memory_type == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        abi->result_parts[index] = (Om_Llvm_Native_Record_Part){
            .llvm_type = i64_type,
            .memory_type = memory_type,
            .offset = (uint64_t)index * 8,
            .memory_size = sizes[index],
            .bit_width = 64,
            .alignment = part_alignment(alignment, (uint64_t)index * 8),
            .mode = OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND,
            .is_integer = true,
        };
    }
    abi->result_part_count = 2;
    return OM_STATUS_OK;
}

Om_Status om_llvm_native_record_classify_aapcs64(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, Om_Llvm_Native_Record_Abi *abi)
{
    return om_llvm_native_record_classify_aapcs64_common(
        context, record_type, size, alignment, fields, field_count, true, abi);
}
