#include "ombasic/foundation/memory.h"

#include <stddef.h>
#include <string.h>

static const char *os_name(void)
{
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

static const char *architecture(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

static char *copy_string(const char *value)
{
    const size_t length = strlen(value);
    char *copy = om_alloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

char *om_runtime_platform_os_name(void)
{
    return copy_string(os_name());
}

char *om_runtime_platform_architecture(void)
{
    return copy_string(architecture());
}

char *om_runtime_platform_target_triple(void)
{
#if defined(_WIN32)
    static const char *vendor = "unknown";
    static const char *system = "windows-msvc";
#elif defined(__linux__)
    static const char *vendor = "unknown";
    static const char *system = "linux-gnu";
#elif defined(__APPLE__)
    static const char *vendor = "apple";
    static const char *system = "darwin";
#else
    static const char *vendor = "unknown";
    static const char *system = "unknown";
#endif
    const size_t arch_length = strlen(architecture());
    const size_t vendor_length = strlen(vendor);
    const size_t system_length = strlen(system);
    const size_t total = arch_length + 1 + vendor_length + 1 + system_length;
    char *triple = om_alloc(total + 1);
    if (triple == NULL) return NULL;
    size_t cursor = 0;
    memcpy(triple + cursor, architecture(), arch_length);
    cursor += arch_length;
    triple[cursor++] = '-';
    memcpy(triple + cursor, vendor, vendor_length);
    cursor += vendor_length;
    triple[cursor++] = '-';
    memcpy(triple + cursor, system, system_length);
    cursor += system_length;
    triple[cursor] = '\0';
    return triple;
}

char *om_runtime_platform_path_separator(void)
{
#if defined(_WIN32)
    return copy_string("\\");
#else
    return copy_string("/");
#endif
}

char *om_runtime_platform_line_separator(void)
{
#if defined(_WIN32)
    return copy_string("\r\n");
#else
    return copy_string("\n");
#endif
}

void om_runtime_platform_release(void *value)
{
    om_free(value);
}
