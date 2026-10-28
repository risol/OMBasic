#ifndef OMBASIC_TARGET_TARGET_H
#define OMBASIC_TARGET_TARGET_H

#include <stdint.h>

#include "ombasic/foundation/status.h"

typedef enum Om_Target_C_Abi_Family {
    OM_TARGET_C_ABI_INVALID = 0,
    OM_TARGET_C_ABI_SYSV_AMD64,
    OM_TARGET_C_ABI_WIN64_X64,
    OM_TARGET_C_ABI_AAPCS64,
    OM_TARGET_C_ABI_DARWIN_ARM64
} Om_Target_C_Abi_Family;

typedef struct Om_Target {
    const char *triple;
    Om_Target_C_Abi_Family c_abi;
    uint8_t pointer_size;
    uint8_t c_long_size;
} Om_Target;

/* Describes the ABI families implemented by the Native FFI backend. */
Om_Status om_target_describe(const char *triple, Om_Target *target);

#endif
