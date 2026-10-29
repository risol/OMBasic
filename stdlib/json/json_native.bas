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
