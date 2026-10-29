IMPORT "./json_error.bas"
IMPORT "./json_native.bas"
IMPORT "./value.bas"

EXPORT FUNCTION stringify(item)
    IF NOT json_native.is_valid(item.encoded) THEN
        RETURN Error(json_error.Parse_Error(json_native.error_code(item.encoded),
                                       json_native.error_offset(item.encoded),
                                       json_native.error_message(item.encoded)))
    END IF
    RETURN Ok(json_native.compact(item.encoded))
END FUNCTION
