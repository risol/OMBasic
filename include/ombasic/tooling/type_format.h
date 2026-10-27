#ifndef OMBASIC_TOOLING_TYPE_FORMAT_H
#define OMBASIC_TOOLING_TYPE_FORMAT_H

#include <stddef.h>

#include "ombasic/foundation/status.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/type_store.h"

typedef struct Om_Tooling_String {
    char *data;
    size_t length;
} Om_Tooling_String;

Om_Status om_tooling_type_to_string(
    const Om_Type_Store *type_store,
    const Om_Resolution_Result *resolution,
    Om_Type_Id type_id,
    Om_Tooling_String *result
);
void om_tooling_string_destroy(Om_Tooling_String *string);

#endif
