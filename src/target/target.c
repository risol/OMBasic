#include "ombasic/target/target.h"

#include <stdbool.h>
#include <string.h>

static bool triple_has(const char *triple, const char *part)
{
    return strstr(triple, part) != NULL;
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

    Om_Target_C_Abi_Family family = OM_TARGET_C_ABI_INVALID;
    uint8_t long_size = 0;
    if (is_x64 && is_windows) {
        family = OM_TARGET_C_ABI_WIN64_X64;
        long_size = 4;
    } else if (is_x64 && (triple_has(triple, "linux")
                          || triple_has(triple, "gnu"))) {
        family = OM_TARGET_C_ABI_SYSV_AMD64;
        long_size = 8;
    } else if (is_arm64 && is_apple) {
        family = OM_TARGET_C_ABI_DARWIN_ARM64;
        long_size = 8;
    } else if (is_arm64 && (triple_has(triple, "linux")
                            || triple_has(triple, "gnu")
                            || triple_has(triple, "android")
                            || triple_has(triple, "freebsd"))) {
        family = OM_TARGET_C_ABI_AAPCS64;
        long_size = 8;
    } else {
        return OM_STATUS_OUT_OF_RANGE;
    }

    *target = (Om_Target){
        .triple = triple,
        .c_abi = family,
        .pointer_size = 8,
        .c_long_size = long_size,
    };
    return OM_STATUS_OK;
}
