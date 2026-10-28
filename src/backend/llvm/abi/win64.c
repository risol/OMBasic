#include "native_abi.h"

#include <limits.h>
#include <string.h>

Om_Status om_llvm_native_record_classify_win64(LLVMContextRef context,
                                               LLVMTypeRef record_type,
                                               uint64_t size,
                                               uint64_t alignment,
                                               Om_Llvm_Native_Record_Abi *abi)
{
    if (context == NULL || record_type == NULL || abi == NULL || size == 0 ||
        alignment == 0)
    {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memset(abi, 0, sizeof(*abi));
    abi->llvm_type = record_type;
    abi->size = size;
    abi->alignment = alignment;
    if (size == 1 || size == 2 || size == 4 || size == 8)
    {
        if (size > UINT_MAX / CHAR_BIT)
        {
            return OM_STATUS_OUT_OF_RANGE;
        }
        abi->parts[0] = (Om_Llvm_Native_Record_Part){
            .llvm_type =
                LLVMIntTypeInContext(context, (unsigned)(size * CHAR_BIT)),
            .offset = 0,
            .bit_width = (unsigned)(size * CHAR_BIT),
            .alignment = (unsigned)alignment,
            .is_integer = true,
        };
        if (abi->parts[0].llvm_type == NULL)
        {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        abi->part_count = 1;
    }
    else
    {
        abi->indirect = true;
    }
    return OM_STATUS_OK;
}
