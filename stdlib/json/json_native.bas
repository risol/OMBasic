NATIVE IMPORT "./json_native.omi"

EXPORT FUNCTION is_valid(source)
    RETURN json_valid(source)
END FUNCTION

EXPORT FUNCTION kind_code(source)
    RETURN json_kind(source)
END FUNCTION

EXPORT FUNCTION error_code(source)
    RETURN json_error_code(source)
END FUNCTION

EXPORT FUNCTION error_offset(source)
    RETURN json_error_offset(source)
END FUNCTION

EXPORT FUNCTION error_message(source)
    RETURN json_error_message(source)
END FUNCTION

EXPORT FUNCTION compact(source)
    RETURN json_compact(source)
END FUNCTION

EXPORT FUNCTION quote(source)
    RETURN json_quote(source)
END FUNCTION

EXPORT FUNCTION array_length(source)
    RETURN json_array_length(source)
END FUNCTION

EXPORT FUNCTION array_index_valid(source, index)
    RETURN json_array_index_valid(source, index)
END FUNCTION

EXPORT FUNCTION array_get(source, index)
    RETURN json_array_get(source, index)
END FUNCTION

EXPORT FUNCTION object_key_valid(source, key)
    RETURN json_object_key_valid(source, key)
END FUNCTION

EXPORT FUNCTION object_get(source, key)
    RETURN json_object_get(source, key)
END FUNCTION

EXPORT FUNCTION string_value(source)
    RETURN json_string_value(source)
END FUNCTION

EXPORT FUNCTION object_length(source)
    RETURN json_object_length(source)
END FUNCTION

EXPORT FUNCTION object_keys(source)
    RETURN json_object_keys(source)
END FUNCTION

EXPORT FUNCTION int64_valid(source)
    RETURN json_int64_valid(source)
END FUNCTION

EXPORT FUNCTION int64_value(source)
    RETURN json_int64_value(source)
END FUNCTION

EXPORT FUNCTION float64_valid(source)
    RETURN json_float64_valid(source)
END FUNCTION

EXPORT FUNCTION float64_value(source)
    RETURN json_float64_value(source)
END FUNCTION
