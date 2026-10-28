#include "ombasic/target/target.h"

#include <stdbool.h>
#include <string.h>

#ifndef OM_TARGET_DEFAULT_TRIPLE
#define OM_TARGET_DEFAULT_TRIPLE ""
#endif

static bool triple_has(const char *triple, const char *part)
{
    return strstr(triple, part) != NULL;
}

Om_Status om_target_host(Om_Target *target)
{
    if (OM_TARGET_DEFAULT_TRIPLE[0] == '\0') {
        return OM_STATUS_NOT_FOUND;
    }
    return om_target_describe(OM_TARGET_DEFAULT_TRIPLE, target);
}

Om_Status om_target_describe(const char *triple, Om_Target *target)
{
    if (triple == NULL || triple[0] == '\0' || target == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    const bool is_x64 = triple_has(triple, "x86_64")
        || triple_has(triple, "amd64");
    const bool is_arm64 = triple_has(triple, "aarch64")
        || triple_has(triple, "arm64");
    const bool is_windows = triple_has(triple, "windows")
        || triple_has(triple, "win32")
        || triple_has(triple, "msvc")
        || triple_has(triple, "mingw");
    const bool is_apple = triple_has(triple, "apple")
        || triple_has(triple, "darwin")
        || triple_has(triple, "macos");
    const bool is_android = triple_has(triple, "android");
    const bool is_freebsd = triple_has(triple, "freebsd");
    const bool is_linux = triple_has(triple, "linux");
    const bool is_msvc = triple_has(triple, "msvc");
    const bool is_gnu = triple_has(triple, "gnu")
        || triple_has(triple, "mingw");
    const bool is_musl = triple_has(triple, "musl");

    Om_Target described = {
        .triple = triple,
        .pointer_size = 8,
        .endianness = OM_TARGET_ENDIANNESS_LITTLE,
    };
    if (is_x64 && is_windows && (is_msvc || is_gnu)) {
        described.architecture = OM_TARGET_ARCH_X86_64;
        described.os = OM_TARGET_OS_WINDOWS;
        described.environment = is_msvc
            ? OM_TARGET_ENVIRONMENT_MSVC : OM_TARGET_ENVIRONMENT_GNU;
        described.object_format = OM_TARGET_OBJECT_FORMAT_COFF;
        described.c_abi = OM_TARGET_C_ABI_WIN64_X64;
        described.c_long_size = 4;
    } else if (is_x64 && is_linux && (is_gnu || is_musl)) {
        described.architecture = OM_TARGET_ARCH_X86_64;
        described.os = OM_TARGET_OS_LINUX;
        described.environment = is_musl
            ? OM_TARGET_ENVIRONMENT_MUSL : OM_TARGET_ENVIRONMENT_GNU;
        described.object_format = OM_TARGET_OBJECT_FORMAT_ELF;
        described.c_abi = OM_TARGET_C_ABI_SYSV_AMD64;
        described.c_long_size = 8;
    } else if (is_arm64 && is_apple) {
        described.architecture = OM_TARGET_ARCH_AARCH64;
        described.os = OM_TARGET_OS_DARWIN;
        described.environment = OM_TARGET_ENVIRONMENT_DARWIN;
        described.object_format = OM_TARGET_OBJECT_FORMAT_MACH_O;
        described.c_abi = OM_TARGET_C_ABI_DARWIN_ARM64;
        described.c_long_size = 8;
    } else if (is_arm64 && (is_linux || is_android || is_freebsd)) {
        if (is_linux && !is_android && !is_gnu && !is_musl) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        described.architecture = OM_TARGET_ARCH_AARCH64;
        described.os = is_android ? OM_TARGET_OS_ANDROID
            : is_freebsd ? OM_TARGET_OS_FREEBSD : OM_TARGET_OS_LINUX;
        described.environment = is_android ? OM_TARGET_ENVIRONMENT_ANDROID
            : is_freebsd ? OM_TARGET_ENVIRONMENT_UNKNOWN
            : is_musl ? OM_TARGET_ENVIRONMENT_MUSL
            : OM_TARGET_ENVIRONMENT_GNU;
        described.object_format = OM_TARGET_OBJECT_FORMAT_ELF;
        described.c_abi = OM_TARGET_C_ABI_AAPCS64;
        described.c_long_size = 8;
    } else {
        return OM_STATUS_OUT_OF_RANGE;
    }

    *target = described;
    return OM_STATUS_OK;
}
