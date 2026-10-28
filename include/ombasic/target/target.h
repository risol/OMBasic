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

typedef enum Om_Target_Architecture {
    OM_TARGET_ARCH_INVALID = 0,
    OM_TARGET_ARCH_X86_64,
    OM_TARGET_ARCH_AARCH64
} Om_Target_Architecture;

typedef enum Om_Target_Os {
    OM_TARGET_OS_INVALID = 0,
    OM_TARGET_OS_WINDOWS,
    OM_TARGET_OS_LINUX,
    OM_TARGET_OS_DARWIN,
    OM_TARGET_OS_ANDROID,
    OM_TARGET_OS_FREEBSD
} Om_Target_Os;

typedef enum Om_Target_Environment {
    OM_TARGET_ENVIRONMENT_INVALID = 0,
    OM_TARGET_ENVIRONMENT_UNKNOWN,
    OM_TARGET_ENVIRONMENT_MSVC,
    OM_TARGET_ENVIRONMENT_GNU,
    OM_TARGET_ENVIRONMENT_MUSL,
    OM_TARGET_ENVIRONMENT_ANDROID,
    OM_TARGET_ENVIRONMENT_DARWIN
} Om_Target_Environment;

typedef enum Om_Target_Object_Format {
    OM_TARGET_OBJECT_FORMAT_INVALID = 0,
    OM_TARGET_OBJECT_FORMAT_COFF,
    OM_TARGET_OBJECT_FORMAT_ELF,
    OM_TARGET_OBJECT_FORMAT_MACH_O
} Om_Target_Object_Format;

typedef enum Om_Target_Endianness {
    OM_TARGET_ENDIANNESS_INVALID = 0,
    OM_TARGET_ENDIANNESS_LITTLE,
    OM_TARGET_ENDIANNESS_BIG
} Om_Target_Endianness;

typedef struct Om_Target {
    const char *triple;
    Om_Target_Architecture architecture;
    Om_Target_Os os;
    Om_Target_Environment environment;
    Om_Target_Object_Format object_format;
    Om_Target_C_Abi_Family c_abi;
    uint8_t pointer_size;
    uint8_t c_long_size;
    Om_Target_Endianness endianness;
} Om_Target;

/* The configured host triple is the default target for this compiler build. */
Om_Status om_target_host(Om_Target *target);

/* Describes supported architecture, platform, object format, and C ABI data. */
Om_Status om_target_describe(const char *triple, Om_Target *target);

#endif
