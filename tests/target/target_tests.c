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

static bool has_platform(const char *triple,
                         Om_Target_Architecture architecture,
                         Om_Target_Os os,
                         Om_Target_Environment environment,
                         Om_Target_Object_Format object_format)
{
    Om_Target target = {0};
    return om_target_describe(triple, &target) == OM_STATUS_OK
        && target.architecture == architecture && target.os == os
        && target.environment == environment
        && target.object_format == object_format
        && target.endianness == OM_TARGET_ENDIANNESS_LITTLE;
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
    if (!has_platform("x86_64-pc-windows-msvc", OM_TARGET_ARCH_X86_64,
                      OM_TARGET_OS_WINDOWS, OM_TARGET_ENVIRONMENT_MSVC,
                      OM_TARGET_OBJECT_FORMAT_COFF)
        || !has_platform("x86_64-unknown-linux-gnu", OM_TARGET_ARCH_X86_64,
                         OM_TARGET_OS_LINUX, OM_TARGET_ENVIRONMENT_GNU,
                         OM_TARGET_OBJECT_FORMAT_ELF)
        || !has_platform("aarch64-unknown-linux-gnu",
                         OM_TARGET_ARCH_AARCH64, OM_TARGET_OS_LINUX,
                         OM_TARGET_ENVIRONMENT_GNU,
                         OM_TARGET_OBJECT_FORMAT_ELF)
        || !has_platform("arm64-apple-darwin", OM_TARGET_ARCH_AARCH64,
                         OM_TARGET_OS_DARWIN, OM_TARGET_ENVIRONMENT_DARWIN,
                         OM_TARGET_OBJECT_FORMAT_MACH_O)
        || !has_platform("aarch64-unknown-linux-android",
                         OM_TARGET_ARCH_AARCH64, OM_TARGET_OS_ANDROID,
                         OM_TARGET_ENVIRONMENT_ANDROID,
                         OM_TARGET_OBJECT_FORMAT_ELF)
        || !has_platform("aarch64-unknown-freebsd",
                         OM_TARGET_ARCH_AARCH64, OM_TARGET_OS_FREEBSD,
                         OM_TARGET_ENVIRONMENT_UNKNOWN,
                         OM_TARGET_OBJECT_FORMAT_ELF)) {
        (void)fprintf(stderr, "target platform classification failed\n");
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
    if (om_target_host(&target) != OM_STATUS_OK
        || target.triple == NULL || target.triple[0] == '\0') {
        (void)fprintf(stderr, "configured host target was unavailable\n");
        return 1;
    }
    return 0;
}
