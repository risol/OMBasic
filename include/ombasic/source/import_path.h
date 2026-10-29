#ifndef OMBASIC_SOURCE_IMPORT_PATH_H
#define OMBASIC_SOURCE_IMPORT_PATH_H

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"

typedef struct Om_Resolved_Import_Path {
    char *data;
    size_t length;
} Om_Resolved_Import_Path;

/* Validate the source-root-relative canonical path form required by §21.4. */
Om_Status om_import_path_validate(Om_String_View path);

/*
 * Decode the two Import literal path forms into one source-root-relative
 * canonical path. The returned path is owned by the result and must be
 * released with om_resolved_import_path_destroy.
 */
Om_Status om_import_path_resolve(Om_String_View importer_canonical_path,
                                 Om_String_View import_literal,
                                 Om_Resolved_Import_Path *out);
void om_resolved_import_path_destroy(Om_Resolved_Import_Path *path);

/* Validate a source-root-relative lower_snake_case path with an exact suffix. */
Om_Status om_source_path_validate(Om_String_View path,
                                  Om_String_View required_extension);

#endif
