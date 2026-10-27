#ifndef OMBASIC_SOURCE_FILE_LOADER_H
#define OMBASIC_SOURCE_FILE_LOADER_H

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/source/source_text.h"

typedef struct Om_Source_Root {
    Om_String_View canonical_path;
    void *owned_storage;
} Om_Source_Root;

/* Pass a zero-initialized root; this canonicalizes and pins its directory. */
Om_Status om_source_root_init(Om_Source_Root *root, Om_String_View path);
void om_source_root_destroy(Om_Source_Root *root);

/* Loads a validated source-root-relative .bas path with escape and case checks. */
Om_Status om_source_root_load(
    const Om_Source_Root *root,
    Om_Source_Id source_id,
    Om_String_View module_path,
    size_t maximum_byte_length,
    Om_Source_Text *source
);

/* Loads a root-relative source file with an exact required extension. */
Om_Status om_source_root_load_file(
    const Om_Source_Root *root,
    Om_Source_Id source_id,
    Om_String_View path,
    Om_String_View required_extension,
    size_t maximum_byte_length,
    Om_Source_Text *source
);

#endif
