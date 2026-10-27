#ifndef OMBASIC_FFI_OMI_PROJECT_H
#define OMBASIC_FFI_OMI_PROJECT_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/ffi/omi.h"
#include "ombasic/source/file_loader.h"

typedef struct Om_Omi_Project {
    const Om_Source_Text *sources;
    const Om_Omi_Module *modules;
    size_t module_count;
    void *owned_storage;
} Om_Omi_Project;

/*
 * Loads the exact `.omi` paths listed by the caller, relative to one source
 * root. Assign source IDs from a range that does not overlap `.bas` modules.
 * Metadata diagnostics remain available on each parsed module.
 */
Om_Status om_omi_project_load(
    const Om_Source_Root *source_root,
    const Om_String_View *metadata_paths,
    size_t metadata_path_count,
    uint32_t first_source_id,
    size_t maximum_source_byte_length,
    Om_Omi_Project *project
);
void om_omi_project_destroy(Om_Omi_Project *project);
const Om_Omi_Module *om_omi_project_module(
    const Om_Omi_Project *project,
    size_t index
);
const Om_Source_Text *om_omi_project_source(
    const Om_Omi_Project *project,
    size_t index
);

#endif
