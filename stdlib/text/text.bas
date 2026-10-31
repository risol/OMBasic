NATIVE IMPORT "./text_native.omi"

EXPORT FUNCTION concat(left, right)
    RETURN (left + right) + ""
END FUNCTION

EXPORT FUNCTION equals(left, right)
    RETURN (left + "") = right
END FUNCTION

EXPORT FUNCTION is_empty(value)
    RETURN value = ""
END FUNCTION

EXPORT FUNCTION length(value)
    RETURN scalar_length(value)
END FUNCTION

EXPORT FUNCTION byte_length(value)
    RETURN scalar_byte_length(value)
END FUNCTION

EXPORT FUNCTION starts_with(value, prefix)
    RETURN scalar_starts_with(value, prefix)
END FUNCTION

EXPORT FUNCTION ends_with(value, suffix)
    RETURN scalar_ends_with(value, suffix)
END FUNCTION

EXPORT FUNCTION find(value, needle)
    RETURN scalar_find(value, needle, 0)
END FUNCTION

EXPORT FUNCTION find_from(value, needle, start_index)
    RETURN scalar_find(value, needle, start_index)
END FUNCTION

EXPORT FUNCTION find_last(value, needle)
    RETURN scalar_find_last(value, needle)
END FUNCTION

EXPORT FUNCTION slice(value, start_index, scalar_count)
    RETURN scalar_slice(value, start_index, scalar_count)
END FUNCTION

EXPORT FUNCTION trim(value)
    RETURN scalar_trim(value, 0)
END FUNCTION

EXPORT FUNCTION trim_start(value)
    RETURN scalar_trim(value, 1)
END FUNCTION

EXPORT FUNCTION trim_end(value)
    RETURN scalar_trim(value, 2)
END FUNCTION

EXPORT FUNCTION split(value, separator)
    RETURN string_split_list(value, separator)
END FUNCTION

EXPORT FUNCTION join(values, separator)
    RETURN string_join_list(values, separator)
END FUNCTION

' Replaces non-overlapping matches from left to right; an empty needle is a no-op.
EXPORT FUNCTION replace(value, needle, replacement)
    RETURN string_replace(value, needle, replacement)
END FUNCTION

