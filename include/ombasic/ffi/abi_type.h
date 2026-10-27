#ifndef OMBASIC_FFI_ABI_TYPE_H
#define OMBASIC_FFI_ABI_TYPE_H

#include <stdbool.h>
#include <stddef.h>

#include "ombasic/ffi/omi.h"

typedef struct Om_Native_Abi_Type_Info {
    size_t size;
    size_t alignment;
    bool is_signed_integer;
    bool is_floating_point;
    bool is_pointer;
} Om_Native_Abi_Type_Info;

typedef struct Om_Native_Record_Layout {
    size_t size;
    size_t alignment;
} Om_Native_Record_Layout;

/* Reports the current process C ABI representation of an .omi ABI type. */
Om_Status om_native_abi_type_info(Om_Native_Abi_Type type,
                                  Om_Native_Abi_Type_Info *info);

/*
 * Computes default C struct layout for the supported scalar and pointer
 * fields. `field_offsets` must hold `field_count` entries. Empty records are
 * rejected because ISO C has no empty structure type.
 */
Om_Status om_native_record_layout(const Om_Omi_Record_Field *fields,
                                  size_t field_count,
                                  size_t *field_offsets,
                                  size_t offset_capacity,
                                  Om_Native_Record_Layout *layout);

#endif
