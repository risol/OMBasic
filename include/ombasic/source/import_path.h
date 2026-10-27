#ifndef OMBASIC_SOURCE_IMPORT_PATH_H
#define OMBASIC_SOURCE_IMPORT_PATH_H

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"

/* Validate the source-root-relative canonical path form required by §21.4. */
Om_Status om_import_path_validate(Om_String_View path);

/* Validate a source-root-relative lower_snake_case path with an exact suffix. */
Om_Status om_source_path_validate(Om_String_View path,
                                  Om_String_View required_extension);

#endif
