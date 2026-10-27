#ifndef OMBASIC_SYNTAX_NAME_RULES_H
#define OMBASIC_SYNTAX_NAME_RULES_H

#include <stdbool.h>

#include "ombasic/foundation/string_view.h"

typedef enum Om_Name_Class {
    OM_NAME_CLASS_INVALID = 0,
    OM_NAME_CLASS_ORDINARY,
    OM_NAME_CLASS_ENTITY
} Om_Name_Class;

Om_Name_Class om_name_classify(Om_String_View name);
bool om_name_equal_case_insensitive(Om_String_View left, Om_String_View right);
bool om_name_is_reserved_builtin(Om_String_View name);

#endif
