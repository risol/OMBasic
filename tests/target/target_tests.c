#include "ombasic/target/target.h"

#include <stdbool.h>
#include <stdio.h>

static bool matches(const char *triple, Om_Target_C_Abi_Family family,
                    uint8_t pointer_size, uint8_t long_size)
{
    Om_Target target = {0};
    return om_target_describe(triple, &target) == OM_STATUS_OK
        && target.triple == triple && target.c_abi == family
        && target.pointer_size == pointer_size
        && target.c_long_size == long_size;
}

int main(void)
{
    if (!matches("x86_64-pc-windows-msvc", OM_TARGET_C_ABI_WIN64_X64, 8, 4)
        || !matches("x86_64-unknown-linux-gnu", OM_TARGET_C_ABI_SYSV_AMD64,
                    8, 8)
        || !matches("aarch64-unknown-linux-gnu", OM_TARGET_C_ABI_AAPCS64,
                    8, 8)
        || !matches("arm64-apple-darwin", OM_TARGET_C_ABI_DARWIN_ARM64,
                    8, 8)) {
        (void)fprintf(stderr, "target triple classification failed\n");
        return 1;
    }
    Om_Target target = {0};
    if (om_target_describe("x86_64-unknown-freebsd", &target)
            != OM_STATUS_OUT_OF_RANGE
        || om_target_describe(NULL, &target) != OM_STATUS_INVALID_ARGUMENT
        || om_target_describe("", &target) != OM_STATUS_INVALID_ARGUMENT) {
        (void)fprintf(stderr, "unsupported target was accepted\n");
        return 1;
    }
    return 0;
}
