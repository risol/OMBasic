#include "ombasic/toolchain/resources.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "ombasic/foundation/memory.h"

#ifndef OM_TOOLCHAIN_TARGET_TRIPLE
#define OM_TOOLCHAIN_TARGET_TRIPLE ""
#endif

static char *copy_string(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    const size_t length = strlen(value);
    if (length == SIZE_MAX) {
        return NULL;
    }
    char *copy = om_alloc(length + 1);
    if (copy != NULL) {
        (void)memcpy(copy, value, length + 1);
    }
    return copy;
}

static char *copy_environment_variable(const char *name)
{
#ifdef _WIN32
    size_t required = 0;
    if (getenv_s(&required, NULL, 0, name) != 0 || required == 0) {
        return NULL;
    }
    char *value = om_alloc(required);
    if (value == NULL) {
        return NULL;
    }
    size_t actual = 0;
    if (getenv_s(&actual, value, required, name) != 0
        || actual > required) {
        om_free(value);
        return NULL;
    }
    return value;
#else
    return copy_string(getenv(name));
#endif
}

static bool is_separator(char value)
{
    return value == '/' || value == '\\';
}

static char *join_path(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return NULL;
    }
    const size_t left_length = strlen(left);
    const size_t right_length = strlen(right);
    const bool needs_separator = left_length != 0
        && !is_separator(left[left_length - 1]);
    if (right_length > SIZE_MAX - left_length - (needs_separator ? 2u : 1u)) {
        return NULL;
    }
    const size_t length = left_length + (needs_separator ? 1u : 0u)
        + right_length;
    char *joined = om_alloc(length + 1);
    if (joined == NULL) {
        return NULL;
    }
    (void)memcpy(joined, left, left_length);
    size_t offset = left_length;
    if (needs_separator) {
        joined[offset++] = '/';
    }
    (void)memcpy(joined + offset, right, right_length);
    joined[length] = '\0';
    return joined;
}

static char *directory_name(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    size_t end = strlen(path);
    while (end > 1 && is_separator(path[end - 1])) {
        --end;
    }
    size_t separator = end;
    while (separator > 0 && !is_separator(path[separator - 1])) {
        --separator;
    }
    if (separator == 0) {
        return copy_string(".");
    }
    size_t length = separator;
    if (length > 1 && is_separator(path[length - 1])) {
        --length;
    }
    if (length == 2 && path[1] == ':') {
        length = 3;
    }
    char *directory = om_alloc(length + 1);
    if (directory == NULL) {
        return NULL;
    }
    (void)memcpy(directory, path, length);
    directory[length] = '\0';
    return directory;
}

static char *current_executable_path(void)
{
#ifdef _WIN32
    DWORD capacity = 512;
    for (;;) {
        char *path = om_alloc((size_t)capacity);
        if (path == NULL) {
            return NULL;
        }
        const DWORD length = GetModuleFileNameA(NULL, path, capacity);
        if (length == 0) {
            om_free(path);
            return NULL;
        }
        if (length < capacity - 1) {
            path[length] = '\0';
            return path;
        }
        om_free(path);
        if (capacity >= 32768) {
            return NULL;
        }
        capacity *= 2;
    }
#else
    size_t capacity = 512;
    for (;;) {
        char *path = om_alloc(capacity + 1);
        if (path == NULL) {
            return NULL;
        }
        const ssize_t length = readlink("/proc/self/exe", path, capacity);
        if (length < 0) {
            om_free(path);
            return NULL;
        }
        if ((size_t)length < capacity) {
            path[length] = '\0';
            return path;
        }
        om_free(path);
        if (capacity > SIZE_MAX / 2) {
            return NULL;
        }
        capacity *= 2;
    }
#endif
}

static bool is_regular_file(const char *path)
{
    struct stat information;
    return path != NULL && stat(path, &information) == 0
        && (information.st_mode & S_IFMT) == S_IFREG;
}

static bool is_directory(const char *path)
{
    struct stat information;
    return path != NULL && stat(path, &information) == 0
        && (information.st_mode & S_IFMT) == S_IFDIR;
}

static const char *const *archive_names(void)
{
#ifdef _WIN32
    static const char *const names[OM_TOOLCHAIN_RUNTIME_ARCHIVE_COUNT] = {
        "ombasic_runtime.lib",
        "ombasic_runtime_services.lib",
        "ombasic_runtime_distribution.lib",
        "ombasic_distribution.lib",
        "ombasic_foundation.lib",
    };
#else
    static const char *const names[OM_TOOLCHAIN_RUNTIME_ARCHIVE_COUNT] = {
        "libombasic_runtime.a",
        "libombasic_runtime_services.a",
        "libombasic_runtime_distribution.a",
        "libombasic_distribution.a",
        "libombasic_foundation.a",
    };
#endif
    return names;
}

void om_toolchain_resources_destroy(Om_Toolchain_Resources *resources)
{
    if (resources == NULL) {
        return;
    }
    om_free(resources->resource_root);
    om_free(resources->target_triple);
    om_free(resources->target_runtime_root);
    om_free(resources->runtime_archive);
    om_free(resources->runtime_services_archive);
    om_free(resources->runtime_distribution_archive);
    om_free(resources->distribution_archive);
    om_free(resources->foundation_archive);
    om_free(resources->missing_archive_path);
    (void)memset(resources, 0, sizeof(*resources));
}

static char **archive_path_slot(Om_Toolchain_Resources *resources,
                                size_t index)
{
    switch (index) {
    case 0: return &resources->runtime_archive;
    case 1: return &resources->runtime_services_archive;
    case 2: return &resources->runtime_distribution_archive;
    case 3: return &resources->distribution_archive;
    case 4: return &resources->foundation_archive;
    default: return NULL;
    }
}

static Om_Status fill_runtime_paths(const char *root,
                                    const char *target_triple,
                                    Om_Toolchain_Resources *resources)
{
    resources->resource_root = copy_string(root);
    resources->target_triple = copy_string(target_triple);
    char *lib_directory = join_path(root, "lib");
    char *ombasic_directory = lib_directory == NULL ? NULL
        : join_path(lib_directory, "ombasic");
    resources->target_runtime_root = ombasic_directory == NULL ? NULL
        : join_path(ombasic_directory, target_triple);
    om_free(lib_directory);
    om_free(ombasic_directory);
    if (resources->resource_root == NULL || resources->target_triple == NULL
        || resources->target_runtime_root == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }

    const char *const *names = archive_names();
    for (size_t index = 0; index < OM_TOOLCHAIN_RUNTIME_ARCHIVE_COUNT; ++index) {
        char **slot = archive_path_slot(resources, index);
        if (slot == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *slot = join_path(resources->target_runtime_root, names[index]);
        if (*slot == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        if (!is_regular_file(*slot)) {
            resources->missing_archive_path = copy_string(*slot);
            return resources->missing_archive_path == NULL
                ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_NOT_FOUND;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status resolve_resource_roots(const char *override,
                                        char **roots,
                                        size_t root_capacity,
                                        size_t *root_count)
{
    if (roots == NULL || root_count == NULL || root_capacity < 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *root_count = 0;
    const char *selected = override;
    char *environment_root = NULL;
    if (selected == NULL || selected[0] == '\0') {
        environment_root = copy_environment_variable("OMBASIC_RESOURCE_DIR");
        selected = environment_root;
    }
    if (selected != NULL && selected[0] != '\0') {
        roots[0] = copy_string(selected);
        om_free(environment_root);
        if (roots[0] == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        *root_count = 1;
        return OM_STATUS_OK;
    }
    om_free(environment_root);

    char *executable = current_executable_path();
    char *executable_directory = executable == NULL
        ? NULL : directory_name(executable);
    char *installed_root = executable_directory == NULL
        ? NULL : directory_name(executable_directory);
    om_free(executable);
    if (executable_directory == NULL || installed_root == NULL) {
        om_free(executable_directory);
        om_free(installed_root);
        return OM_STATUS_NOT_FOUND;
    }
    roots[(*root_count)++] = installed_root;
    if (strcmp(executable_directory, installed_root) != 0) {
        roots[(*root_count)++] = executable_directory;
    } else {
        om_free(executable_directory);
    }
    return OM_STATUS_OK;
}

Om_Status om_toolchain_runtime_resources_resolve(
    const char *resource_dir_override,
    const char *target_triple,
    Om_Toolchain_Resources *resources)
{
    if (target_triple == NULL || target_triple[0] == '\0') {
        target_triple = OM_TOOLCHAIN_TARGET_TRIPLE;
    }
    if (resources == NULL || target_triple[0] == '\0') {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memset(resources, 0, sizeof(*resources));
    char *roots[2] = {NULL, NULL};
    size_t root_count = 0;
    Om_Status status = resolve_resource_roots(resource_dir_override, roots,
                                               2, &root_count);
    if (status != OM_STATUS_OK) {
        return status;
    }

    Om_Toolchain_Resources last_failure = {0};
    for (size_t index = 0; index < root_count; ++index) {
        Om_Toolchain_Resources candidate = {0};
        if (!is_directory(roots[index])) {
            candidate.resource_root = copy_string(roots[index]);
            candidate.target_triple = copy_string(target_triple);
            char *lib_directory = join_path(roots[index], "lib");
            char *ombasic_directory = lib_directory == NULL ? NULL
                : join_path(lib_directory, "ombasic");
            candidate.target_runtime_root = ombasic_directory == NULL ? NULL
                : join_path(ombasic_directory, target_triple);
            if (candidate.target_runtime_root != NULL) {
                const char *const *names = archive_names();
                char *archive_directory = copy_string(
                    candidate.target_runtime_root);
                candidate.missing_archive_path = archive_directory == NULL
                    ? NULL : join_path(archive_directory, names[0]);
                om_free(archive_directory);
            }
            om_free(lib_directory);
            om_free(ombasic_directory);
            status = candidate.resource_root == NULL
                || candidate.target_triple == NULL
                ? OM_STATUS_OUT_OF_MEMORY
                : candidate.missing_archive_path == NULL
                ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_NOT_FOUND;
        } else {
            status = fill_runtime_paths(roots[index], target_triple,
                                        &candidate);
        }
        if (status == OM_STATUS_OK) {
            *resources = candidate;
            for (size_t root_index = 0; root_index < root_count; ++root_index) {
                om_free(roots[root_index]);
            }
            om_toolchain_resources_destroy(&last_failure);
            return OM_STATUS_OK;
        }
        if (status == OM_STATUS_OUT_OF_MEMORY) {
            om_toolchain_resources_destroy(&candidate);
            om_toolchain_resources_destroy(&last_failure);
            for (size_t root_index = 0; root_index < root_count; ++root_index) {
                om_free(roots[root_index]);
            }
            return status;
        }
        om_toolchain_resources_destroy(&last_failure);
        last_failure = candidate;
    }
    *resources = last_failure;
    for (size_t root_index = 0; root_index < root_count; ++root_index) {
        om_free(roots[root_index]);
    }
    return status;
}

Om_Status om_toolchain_resource_path(const char *relative_path,
                                     char **path_out)
{
    if (relative_path == NULL || relative_path[0] == '\0' || path_out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *path_out = NULL;
    char *roots[2] = {NULL, NULL};
    size_t root_count = 0;
    Om_Status status = resolve_resource_roots(NULL, roots, 2, &root_count);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = OM_STATUS_NOT_FOUND;
    for (size_t index = 0; index < root_count; ++index) {
        char *candidate = join_path(roots[index], relative_path);
        if (candidate == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        if (is_regular_file(candidate)) {
            *path_out = candidate;
            status = OM_STATUS_OK;
            break;
        }
        om_free(candidate);
    }
    for (size_t index = 0; index < root_count; ++index) {
        om_free(roots[index]);
    }
    return status;
}
