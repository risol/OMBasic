#ifndef OMBASIC_LINKER_LINKER_H
#define OMBASIC_LINKER_LINKER_H

#include <stddef.h>

#include "ombasic/foundation/string_view.h"
#include "ombasic/toolchain/resources.h"

typedef struct Om_Linker_Request {
    const char *object_path;
    const char *executable_path;
    const Om_String_View *libraries;
    size_t library_count;
    const Om_String_View *library_directories;
    size_t library_directory_count;
    const Om_Toolchain_Resources *resources;
} Om_Linker_Request;

/* Returns the child exit code, or -1 when the process cannot be started. */
int om_linker_run_executable(const char *executable,
                             char *const arguments[]);

/* On MSVC targets, debug_symbols_path_out receives an om_alloc-owned PDB path. */
int om_linker_link_native(const Om_Linker_Request *request,
                          char **debug_symbols_path_out);

#endif
