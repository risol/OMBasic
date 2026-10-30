IMPORT "./json_error.bas"
IMPORT "./json_native.bas"
IMPORT "./value.bas"
IMPORT "@std/text/text.bas"

FUNCTION parse_error(source)
    RETURN json_error.Parse_Error(json_native.error_code(source),
                             json_native.error_offset(source),
                             json_native.error_message(source))
END FUNCTION

FUNCTION decode_key_array(encoded_keys)
    VAR output = text.split("", "")
    count = json_native.array_length(encoded_keys)
    VAR index = 0
    WHILE index < count
        encoded_key = json_native.array_get(encoded_keys, index)
        output = output + [json_native.string_value(encoded_key)]
        index = index + 1
    END WHILE
    RETURN output
END FUNCTION

EXPORT FUNCTION parse(source)
    IF json_native.is_valid(source) THEN
        RETURN Ok(value.Value(json_native.compact(source)))
    END IF
    RETURN Error(parse_error(source))
END FUNCTION

EXPORT FUNCTION kind(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    code = json_native.kind_code(item.encoded)
    IF code = 0 THEN
        RETURN Ok("null")
    ELSE IF code = 1 THEN
        RETURN Ok("boolean")
    ELSE IF code = 2 THEN
        RETURN Ok("number")
    ELSE IF code = 3 THEN
        RETURN Ok("string")
    ELSE IF code = 4 THEN
        RETURN Ok("array")
    ELSE IF code = 5 THEN
        RETURN Ok("object")
    END IF
    RETURN Error(json_error.Parse_Error(6, 0, "unknown JSON value kind"))
END FUNCTION

EXPORT FUNCTION array_length(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 4 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON array"))
    END IF
    RETURN Ok(json_native.array_length(item.encoded))
END FUNCTION

EXPORT FUNCTION as_string(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 3 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON string"))
    END IF
    RETURN Ok(json_native.string_value(item.encoded))
END FUNCTION

EXPORT FUNCTION as_boolean(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 1 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON boolean"))
    END IF
    RETURN Ok(item.encoded = "true")
END FUNCTION

EXPORT FUNCTION as_number_text(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 2 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON number"))
    END IF
    RETURN Ok(item.encoded)
END FUNCTION

EXPORT FUNCTION as_int64(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 2 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON number"))
    END IF
    IF NOT json_native.int64_valid(item.encoded) THEN
        RETURN Error(json_error.Parse_Error(6, 0,
            "JSON number is not an in-range integer token"))
    END IF
    RETURN Ok(json_native.int64_value(item.encoded))
END FUNCTION

EXPORT FUNCTION as_float64(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 2 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON number"))
    END IF
    IF NOT json_native.float64_valid(item.encoded) THEN
        RETURN Error(json_error.Parse_Error(6, 0,
            "JSON number is outside the finite Float64 range"))
    END IF
    RETURN Ok(json_native.float64_value(item.encoded))
END FUNCTION

EXPORT FUNCTION is_null(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    RETURN Ok(json_native.kind_code(item.encoded) = 0)
END FUNCTION

EXPORT FUNCTION object_has(item, key)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 5 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON object"))
    END IF
    RETURN Ok(json_native.object_key_valid(item.encoded, key))
END FUNCTION

EXPORT FUNCTION object_length(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 5 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON object"))
    END IF
    RETURN Ok(json_native.object_length(item.encoded))
END FUNCTION

EXPORT FUNCTION object_keys(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 5 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON object"))
    END IF
    RETURN Ok(decode_key_array(json_native.object_keys(item.encoded)))
END FUNCTION

EXPORT FUNCTION object_entries(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 5 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON object"))
    END IF
    keys = decode_key_array(json_native.object_keys(item.encoded))
    VAR output = []
    FOR key IN keys
        encoded_value = json_native.object_get(item.encoded, key)
        output = output + [value.Entry(key, value.Value(encoded_value))]
    END FOR
    RETURN Ok(output)
END FUNCTION


EXPORT FUNCTION array_get(item, index)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 4 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON array"))
    END IF
    IF NOT json_native.array_index_valid(item.encoded, index) THEN
        RETURN Error(json_error.Parse_Error(6, index, "array index is out of range"))
    END IF
    RETURN Ok(value.Value(json_native.array_get(item.encoded, index)))
END FUNCTION

EXPORT FUNCTION object_get(item, key)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(parse_error(item.encoded))
    END IF
    IF json_native.kind_code(item.encoded) <> 5 THEN
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON object"))
    END IF
    IF NOT json_native.object_key_valid(item.encoded, key) THEN
        RETURN Error(json_error.Parse_Error(6, 0, "object key was not found"))
    END IF
    RETURN Ok(value.Value(json_native.object_get(item.encoded, key)))
END FUNCTION
