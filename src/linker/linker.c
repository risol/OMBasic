#include "ombasic/linker/linker.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef OM_NATIVE_LINK_DRIVER
#define OM_NATIVE_LINK_DRIVER "clang-cl"
#endif
#ifndef OM_LLD_LINKER_DIRECTORY
#define OM_LLD_LINKER_DIRECTORY ""
#endif
#ifndef OM_NATIVE_LINK_DRIVER_STYLE_MSVC
#define OM_NATIVE_LINK_DRIVER_STYLE_MSVC 0
#endif
#ifndef OM_NATIVE_USE_LLD
#define OM_NATIVE_USE_LLD 0
#endif
#ifndef OM_NATIVE_LINK_SANITIZER_FLAG
#define OM_NATIVE_LINK_SANITIZER_FLAG ""
#endif
#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
static char *make_native_pdb_path(const char *executable_path);
#endif

int om_linker_run_executable(const char *executable, char *const arguments[])
{
#ifdef _WIN32
    const intptr_t result = _spawnvp(_P_WAIT, executable,
                                     (const char *const *)arguments);
    return result < 0 || result > INT_MAX ? -1 : (int)result;
#else
    const pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        execvp(executable, arguments);
        _exit(127);
    }
    int child_status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return -1;
    }
    if (WIFEXITED(child_status)) {
        return WEXITSTATUS(child_status);
    }
    if (WIFSIGNALED(child_status)) {
        return 128 + WTERMSIG(child_status);
    }
    return -1;
#endif
}

static char *make_link_argument(const char *prefix,
                                Om_String_View value,
                                const char *suffix)
{
    const size_t prefix_length = prefix == NULL ? 0 : strlen(prefix);
    const size_t suffix_length = suffix == NULL ? 0 : strlen(suffix);
    if (!om_string_view_is_valid(value)
        || value.length > SIZE_MAX - prefix_length
        || value.length + prefix_length > SIZE_MAX - suffix_length - 1) {
        return NULL;
    }
    const size_t length = prefix_length + value.length + suffix_length;
    char *argument = om_alloc(length + 1);
    if (argument == NULL) {
        return NULL;
    }
    size_t offset = 0;
    if (prefix_length != 0) {
        (void)memcpy(argument, prefix, prefix_length);
        offset += prefix_length;
    }
    if (value.length != 0) {
        (void)memcpy(argument + offset, value.data, value.length);
        offset += value.length;
    }
    if (suffix_length != 0) {
        (void)memcpy(argument + offset, suffix, suffix_length);
        offset += suffix_length;
    }
    argument[offset] = '\0';
    return argument;
}

static bool native_library_has_suffix(Om_String_View library,
                                      const char *suffix)
{
    if (!om_string_view_is_valid(library) || suffix == NULL) {
        return false;
    }
    const size_t suffix_length = strlen(suffix);
    if (library.length < suffix_length) {
        return false;
    }
    const size_t start = library.length - suffix_length;
    for (size_t index = 0; index < suffix_length; ++index) {
        unsigned char actual = (unsigned char)library.data[start + index];
        unsigned char expected = (unsigned char)suffix[index];
        if (actual >= 'A' && actual <= 'Z') {
            actual = (unsigned char)(actual + ('a' - 'A'));
        }
        if (expected >= 'A' && expected <= 'Z') {
            expected = (unsigned char)(expected + ('a' - 'A'));
        }
        if (actual != expected) {
            return false;
        }
    }
    return true;
}

static bool native_library_is_path(Om_String_View library)
{
    if (!om_string_view_is_valid(library)) {
        return false;
    }
    for (size_t index = 0; index < library.length; ++index) {
        if (library.data[index] == '/' || library.data[index] == '\\') {
            return true;
        }
    }
    return native_library_has_suffix(library, ".a")
        || native_library_has_suffix(library, ".so")
        || native_library_has_suffix(library, ".lib");
}

#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
static bool native_library_has_lib_suffix(Om_String_View library)
{
    return native_library_has_suffix(library, ".lib");
}
#endif

int om_linker_link_native(const Om_Linker_Request *request,
                          char **debug_symbols_path_out)
{
    if (request == NULL || request->target == NULL
        || request->target->triple == NULL
        || request->target_runtime_root == NULL
        || request->resources == NULL
        || request->resources->target_triple == NULL
        || request->resources->target_runtime_root == NULL
        || strcmp(request->target->triple,
                  request->resources->target_triple) != 0
        || strcmp(request->target_runtime_root,
                  request->resources->target_runtime_root) != 0
        || request->object_path == NULL
        || request->object_path[0] == '\0'
        || request->executable_path == NULL
        || request->executable_path[0] == '\0'
        || request->resources->runtime_archive == NULL
        || request->resources->runtime_services_archive == NULL
        || request->resources->runtime_distribution_archive == NULL
        || request->resources->distribution_archive == NULL
        || request->resources->foundation_archive == NULL
        || (request->library_count != 0 && request->libraries == NULL)
        || (request->library_directory_count != 0
            && request->library_directories == NULL)) {
        return -1;
    }
    if (debug_symbols_path_out != NULL) {
        *debug_symbols_path_out = NULL;
    }
    const char *object_path = request->object_path;
    const char *executable_path = request->executable_path;
    const Om_String_View *libraries = request->libraries;
    const size_t native_module_count = request->library_count;
    const Om_String_View *library_directories = request->library_directories;
    const size_t library_directory_count = request->library_directory_count;
    const Om_Toolchain_Resources *resources = request->resources;
    const char *link_driver = request->link_driver;
    if (link_driver == NULL) {
        link_driver = OM_NATIVE_LINK_DRIVER;
    }
    if (link_driver[0] == '\0') {
        return -1;
    }
    if (library_directory_count > (SIZE_MAX - 36) / 2
        || native_module_count > SIZE_MAX - 36
            - library_directory_count * 2
        || native_module_count > SIZE_MAX - library_directory_count) {
        return -1;
    }
    const size_t argument_capacity = 36 + library_directory_count * 2
        + native_module_count;
    if (library_directory_count > SIZE_MAX - native_module_count
        || (request->sysroot != NULL && !OM_NATIVE_LINK_DRIVER_STYLE_MSVC
            && library_directory_count + native_module_count == SIZE_MAX)) {
        return -1;
    }
    const size_t owned_capacity = library_directory_count + native_module_count
        + (request->sysroot != NULL && !OM_NATIVE_LINK_DRIVER_STYLE_MSVC ? 1 : 0);
    char **arguments = om_alloc_array(argument_capacity, sizeof(*arguments));
    char **owned_arguments = owned_capacity == 0 ? NULL
        : om_alloc_array(owned_capacity, sizeof(*owned_arguments));
    if (arguments == NULL || (owned_capacity != 0 && owned_arguments == NULL)) {
        om_free(arguments);
        om_free(owned_arguments);
        return -1;
    }
    (void)memset(arguments, 0, argument_capacity * sizeof(*arguments));
    size_t count = 0;
    size_t owned_count = 0;
    char *output_argument = NULL;
#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
    char *debug_symbols_path = NULL;
    char *debug_symbols_argument = NULL;
#endif
    arguments[count++] = (char *)link_driver;
#if OM_NATIVE_USE_LLD
    arguments[count++] = "-fuse-ld=lld";
#endif
#if !OM_NATIVE_LINK_DRIVER_STYLE_MSVC
    if (OM_NATIVE_LINK_SANITIZER_FLAG[0] != '\0') {
        arguments[count++] = (char *)OM_NATIVE_LINK_SANITIZER_FLAG;
    }
    for (size_t index = 0; index < library_directory_count; ++index) {
        if (!om_string_view_is_valid(library_directories[index])
            || library_directories[index].length == 0
            || memchr(library_directories[index].data, '\0',
                      library_directories[index].length) != NULL) {
            goto argument_failure;
        }
        arguments[count++] = "-L";
        char *library_path_argument = make_link_argument(NULL,
            library_directories[index], NULL);
        if (library_path_argument == NULL) {
            goto argument_failure;
        }
        arguments[count++] = library_path_argument;
        owned_arguments[owned_count++] = library_path_argument;
    }
#endif
#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
    if (request->sysroot != NULL) {
        if (request->sysroot[0] == '\0') {
            goto argument_failure;
        }
        arguments[count++] = "/winsysroot";
        arguments[count++] = (char *)request->sysroot;
    }
    const size_t output_length = strlen(executable_path);
    if (output_length > SIZE_MAX - sizeof("/Fe")) {
        goto argument_failure;
    }
    output_argument = om_alloc(output_length + sizeof("/Fe"));
    if (output_argument == NULL) {
        goto argument_failure;
    }
    (void)memcpy(output_argument, "/Fe", sizeof("/Fe") - 1);
    (void)memcpy(output_argument + sizeof("/Fe") - 1,
                 executable_path, output_length + 1);
    arguments[count++] = (char *)object_path;
    arguments[count++] = output_argument;
    arguments[count++] = "/link";
    for (size_t index = 0; index < library_directory_count; ++index) {
        if (!om_string_view_is_valid(library_directories[index])
            || library_directories[index].length == 0
            || memchr(library_directories[index].data, '\0',
                      library_directories[index].length) != NULL) {
            goto argument_failure;
        }
        char *library_path_argument = make_link_argument("/LIBPATH:",
            library_directories[index], NULL);
        if (library_path_argument == NULL) {
            goto argument_failure;
        }
        arguments[count++] = library_path_argument;
        owned_arguments[owned_count++] = library_path_argument;
    }
    arguments[count++] = resources->runtime_distribution_archive;
    arguments[count++] = resources->distribution_archive;
    arguments[count++] = resources->runtime_archive;
    arguments[count++] = resources->runtime_services_archive;
    arguments[count++] = resources->foundation_archive;
    arguments[count++] = "/defaultlib:Ws2_32.lib";
    arguments[count++] = "/defaultlib:Bcrypt.lib";
    arguments[count++] = "/defaultlib:libcmt.lib";
    arguments[count++] = "/defaultlib:libvcruntime.lib";
    arguments[count++] = "/defaultlib:libucrt.lib";
    arguments[count++] = "/defaultlib:oldnames.lib";
    debug_symbols_path = make_native_pdb_path(executable_path);
    if (debug_symbols_path == NULL) {
        goto argument_failure;
    }
    const size_t debug_symbols_path_length = strlen(debug_symbols_path);
    if (debug_symbols_path_length > SIZE_MAX - sizeof("/PDB:")) {
        goto argument_failure;
    }
    debug_symbols_argument = om_alloc(debug_symbols_path_length
                                       + sizeof("/PDB:"));
    if (debug_symbols_argument == NULL) {
        goto argument_failure;
    }
    (void)memcpy(debug_symbols_argument, "/PDB:", sizeof("/PDB:") - 1);
    (void)memcpy(debug_symbols_argument + sizeof("/PDB:") - 1,
        debug_symbols_path, debug_symbols_path_length + 1);
    arguments[count++] = "/DEBUG";
    arguments[count++] = debug_symbols_argument;
#else
    if (request->sysroot != NULL) {
        if (request->sysroot[0] == '\0') {
            goto argument_failure;
        }
        char *sysroot_argument = make_link_argument("--sysroot=",
            (Om_String_View){request->sysroot, strlen(request->sysroot)}, NULL);
        if (sysroot_argument == NULL) {
            goto argument_failure;
        }
        arguments[count++] = sysroot_argument;
        owned_arguments[owned_count++] = sysroot_argument;
    }
    if (OM_LLD_LINKER_DIRECTORY[0] != '\0') {
        arguments[count++] = "-B";
        arguments[count++] = (char *)OM_LLD_LINKER_DIRECTORY;
    }
    arguments[count++] = "-L";
    arguments[count++] = resources->target_runtime_root;
    arguments[count++] = (char *)object_path;
    arguments[count++] = resources->runtime_distribution_archive;
    arguments[count++] = resources->distribution_archive;
    arguments[count++] = resources->runtime_archive;
    arguments[count++] = resources->runtime_services_archive;
    arguments[count++] = resources->foundation_archive;
#endif
    for (size_t index = 0; index < native_module_count; ++index) {
        const Om_String_View library = libraries[index];
        if (!om_string_view_is_valid(library) || library.length == 0
            || memchr(library.data, '\0', library.length) != NULL) {
            goto argument_failure;
        }
        const bool is_path = native_library_is_path(library);
#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
        const bool has_lib_suffix = native_library_has_lib_suffix(
            library);
        char *library_argument = is_path
            ? make_link_argument(NULL, library, NULL)
            : make_link_argument("/DEFAULTLIB:", library,
                has_lib_suffix ? NULL : ".lib");
#else
        char *library_argument = is_path
            ? make_link_argument(NULL, library, NULL)
            : make_link_argument("-l", library, NULL);
#endif
        if (library_argument == NULL) {
            goto argument_failure;
        }
        arguments[count++] = library_argument;
        owned_arguments[owned_count++] = library_argument;
    }
#if !OM_NATIVE_LINK_DRIVER_STYLE_MSVC
    arguments[count++] = "-o";
    arguments[count++] = (char *)executable_path;
#endif
    arguments[count] = NULL;
    const int result = om_linker_run_executable(link_driver, arguments);
    om_free(output_argument);
#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
    om_free(debug_symbols_argument);
    if (debug_symbols_path_out != NULL) {
        *debug_symbols_path_out = debug_symbols_path;
    } else {
        om_free(debug_symbols_path);
    }
#endif
    for (size_t index = 0; index < owned_count; ++index) {
        om_free(owned_arguments[index]);
    }
    om_free(owned_arguments);
    om_free(arguments);
    return result;

argument_failure:
    om_free(output_argument);
#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
    om_free(debug_symbols_argument);
    om_free(debug_symbols_path);
#endif
    for (size_t index = 0; index < owned_count; ++index) {
        om_free(owned_arguments[index]);
    }
    om_free(owned_arguments);
    om_free(arguments);
    return -1;
}


#if OM_NATIVE_LINK_DRIVER_STYLE_MSVC
static char *make_native_pdb_path(const char *executable_path)
{
    const size_t length = strlen(executable_path);
    if (length > SIZE_MAX - sizeof(".pdb")) {
        return NULL;
    }
    char *path = om_alloc(length + sizeof(".pdb"));
    if (path == NULL) {
        return NULL;
    }
    (void)memcpy(path, executable_path, length);
    (void)memcpy(path + length, ".pdb", sizeof(".pdb"));
    return path;
}
#endif
