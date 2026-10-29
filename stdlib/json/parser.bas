IMPORT "./json_error.bas"
IMPORT "./json_native.bas"
IMPORT "./value.bas"

FUNCTION parse_error(source)
    RETURN json_error.Parse_Error(json_native.error_code(source),
                             json_native.error_offset(source),
                             json_native.error_message(source))
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
