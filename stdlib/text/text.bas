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

EXPORT FUNCTION split(value, separator)
    value_length = length(value)
    separator_length = length(separator)
    IF separator_length = 0 THEN
        VAR chars = []
        VAR index = 0
        WHILE index < value_length
            chars = chars + [slice(value, index, 1)]
            index = index + 1
        END WHILE
        RETURN chars
    END IF
    VAR parts = []
    VAR start_index = 0
    VAR match_index = -1
    WHILE start_index <= value_length
        match_index = find_from(value, separator, start_index)
        IF match_index < 0 THEN
            RETURN parts + [slice(value, start_index, value_length - start_index)]
        END IF
        parts = parts + [slice(value, start_index, match_index - start_index)]
        start_index = match_index + separator_length
    END WHILE
    RETURN parts
END FUNCTION

EXPORT FUNCTION join(values, separator)
    VAR joined_text = ""
    FOR item IN values
        joined_text = (joined_text + item) + separator
    END FOR
    IF is_empty(joined_text) THEN
        RETURN ""
    END IF
    separator_length = length(separator)
    RETURN slice(joined_text, 0, length(joined_text) - separator_length)
END FUNCTION

