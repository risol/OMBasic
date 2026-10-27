#include "ombasic/syntax/name_rules.h"

#include <stdint.h>

static bool ascii_lower(char byte)
{
    return byte >= 'a' && byte <= 'z';
}

static bool ascii_upper(char byte)
{
    return byte >= 'A' && byte <= 'Z';
}

static bool ascii_digit(char byte)
{
    return byte >= '0' && byte <= '9';
}

static char ascii_fold(char byte)
{
    return ascii_upper(byte) ? (char)(byte + ('a' - 'A')) : byte;
}

static bool matches_ordinary(const char *bytes, size_t length)
{
    if (length == 0 || !ascii_lower(bytes[0])) {
        return false;
    }
    size_t index = 1;
    while (index < length) {
        if (ascii_lower(bytes[index]) || ascii_digit(bytes[index])) {
            ++index;
        } else if (bytes[index] == '_' && index + 1 < length
                   && ascii_lower(bytes[index + 1])) {
            index += 2;
        } else {
            return false;
        }
    }
    return true;
}

static bool matches_entity(const char *bytes, size_t length)
{
    if (length == 0 || !ascii_upper(bytes[0])) {
        return false;
    }
    size_t index = 1;
    while (index < length) {
        if (ascii_lower(bytes[index]) || ascii_digit(bytes[index])) {
            ++index;
        } else if (bytes[index] == '_' && index + 1 < length
                   && ascii_upper(bytes[index + 1])) {
            index += 2;
        } else {
            return false;
        }
    }
    return true;
}

Om_Name_Class om_name_classify(Om_String_View name)
{
    if (name.data == NULL && name.length != 0) {
        return OM_NAME_CLASS_INVALID;
    }
    if (matches_ordinary(name.data, name.length)) {
        return OM_NAME_CLASS_ORDINARY;
    }
    if (matches_entity(name.data, name.length)) {
        return OM_NAME_CLASS_ENTITY;
    }
    return OM_NAME_CLASS_INVALID;
}

bool om_name_equal_case_insensitive(Om_String_View left, Om_String_View right)
{
    if (left.length != right.length || (left.data == NULL && left.length != 0)
        || (right.data == NULL && right.length != 0)) {
        return false;
    }
    for (size_t index = 0; index < left.length; ++index) {
        if (ascii_fold(left.data[index]) != ascii_fold(right.data[index])) {
            return false;
        }
    }
    return true;
}

bool om_name_is_reserved_builtin(Om_String_View name)
{
    static const char *const builtins[] = {
        "Unit", "Result", "Node_Ref", "Actor_Id", "Down_Reason",
        "Ok", "Error", "Actor_Down"
    };
    for (size_t index = 0; index < sizeof(builtins) / sizeof(builtins[0]); ++index) {
        size_t length = 0;
        while (builtins[index][length] != '\0') {
            ++length;
        }
        if (om_name_equal_case_insensitive(name,
                (Om_String_View){builtins[index], length})) {
            return true;
        }
    }
    return false;
}
