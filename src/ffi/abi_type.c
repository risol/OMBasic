#include "ombasic/ffi/abi_type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ABI_CASE(abi, c_type, signed_value, float_value, pointer_value)          \
    case abi:                                                                    \
        *info = (Om_Native_Abi_Type_Info){                                       \
            .size = sizeof(c_type),                                              \
            .alignment = _Alignof(c_type),                                       \
            .is_signed_integer = signed_value,                                   \
            .is_floating_point = float_value,                                   \
            .is_pointer = pointer_value                                          \
        };                                                                       \
        return OM_STATUS_OK

Om_Status om_native_abi_type_info(Om_Native_Abi_Type type,
                                  Om_Native_Abi_Type_Info *info)
{
    if (info == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *info = (Om_Native_Abi_Type_Info){0};
    switch (type) {
    case OM_NATIVE_ABI_CVOID:
        info->alignment = 1;
        return OM_STATUS_OK;
    ABI_CASE(OM_NATIVE_ABI_CINT8, int8_t, true, false, false);
    ABI_CASE(OM_NATIVE_ABI_CINT16, int16_t, true, false, false);
    ABI_CASE(OM_NATIVE_ABI_CINT32, int32_t, true, false, false);
    ABI_CASE(OM_NATIVE_ABI_CINT64, int64_t, true, false, false);
    ABI_CASE(OM_NATIVE_ABI_CUINT8, uint8_t, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CUINT16, uint16_t, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CUINT32, uint32_t, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CUINT64, uint64_t, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CINT, int, true, false, false);
    ABI_CASE(OM_NATIVE_ABI_CUINT, unsigned int, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CLONG, long, true, false, false);
    ABI_CASE(OM_NATIVE_ABI_CULONG, unsigned long, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CLONGLONG, long long, true, false, false);
    ABI_CASE(OM_NATIVE_ABI_CULONGLONG, unsigned long long, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CFLOAT, float, false, true, false);
    ABI_CASE(OM_NATIVE_ABI_CDOUBLE, double, false, true, false);
    ABI_CASE(OM_NATIVE_ABI_CSIZE, size_t, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CBOOL, bool, false, false, false);
    ABI_CASE(OM_NATIVE_ABI_CPOINTER, void *, false, false, true);
    ABI_CASE(OM_NATIVE_ABI_CSTRING, char *, false, false, true);
    case OM_NATIVE_ABI_INVALID:
    case OM_NATIVE_ABI_RECORD:
        break;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static bool align_offset(size_t value, size_t alignment, size_t *aligned)
{
    if (alignment == 0) {
        return false;
    }
    const size_t remainder = value % alignment;
    const size_t padding = remainder == 0 ? 0 : alignment - remainder;
    if (value > SIZE_MAX - padding) {
        return false;
    }
    *aligned = value + padding;
    return true;
}

Om_Status om_native_record_layout(const Om_Omi_Record_Field *fields,
                                  size_t field_count,
                                  size_t *field_offsets,
                                  size_t offset_capacity,
                                  Om_Native_Record_Layout *layout)
{
    if (fields == NULL || field_count == 0 || field_offsets == NULL
        || offset_capacity < field_count || layout == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *layout = (Om_Native_Record_Layout){0};
    size_t offset = 0;
    size_t structure_alignment = 1;
    for (size_t index = 0; index < field_count; ++index) {
        Om_Native_Abi_Type_Info info = {0};
        const Om_Status status = om_native_abi_type_info(
            fields[index].abi_type, &info);
        if (status != OM_STATUS_OK || info.size == 0
            || fields[index].abi_type == OM_NATIVE_ABI_CVOID) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (info.alignment > structure_alignment) {
            structure_alignment = info.alignment;
        }
        if (!align_offset(offset, info.alignment, &offset)) {
            return OM_STATUS_OVERFLOW;
        }
        field_offsets[index] = offset;
        if (offset > SIZE_MAX - info.size) {
            return OM_STATUS_OVERFLOW;
        }
        offset += info.size;
    }
    if (!align_offset(offset, structure_alignment, &layout->size)) {
        return OM_STATUS_OVERFLOW;
    }
    layout->alignment = structure_alignment;
    return OM_STATUS_OK;
}

#undef ABI_CASE
