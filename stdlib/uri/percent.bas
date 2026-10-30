IMPORT "./uri_error.bas"
IMPORT "./uri_model.bas"
IMPORT "@std/collections/list.bas"
IMPORT "@std/text/text.bas"
NATIVE IMPORT "./uri_native.omi"

FUNCTION component_code(component)
    IF component = "path" THEN
        RETURN 1
    END IF
    IF component = "query" THEN
        RETURN 2
    END IF
    IF component = "fragment" THEN
        RETURN 3
    END IF
    IF component = "userinfo" THEN
        RETURN 4
    END IF
    IF component = "query_field" THEN
        RETURN 5
    END IF
    RETURN 0
END FUNCTION

EXPORT FUNCTION percent_encode(value, component)
    code = component_code(component)
    IF code = 0 THEN
        RETURN Error(uri_error.Uri_Error("invalid_component", 0))
    END IF
    status = percent_encode_status(value, code)
    IF status = 3 THEN
        RETURN Error(uri_error.Uri_Error("resource_exhausted", 0))
    END IF
    IF status <> 0 THEN
        RETURN Error(uri_error.Uri_Error("invalid_component", 0))
    END IF
    RETURN Ok(native_percent_encode(value, code))
END FUNCTION

EXPORT FUNCTION percent_decode(value)
    status = percent_decode_status(value)
    IF status = 3 THEN
        RETURN Error(uri_error.Uri_Error("resource_exhausted", 0))
    END IF
    IF status = 1 THEN
        RETURN Error(uri_error.Uri_Error("invalid_percent_encoding",
            percent_decode_offset(value)))
    END IF
    IF status = 2 THEN
        RETURN Error(uri_error.Uri_Error("invalid_utf8",
            percent_decode_offset(value)))
    END IF
    IF status <> 0 THEN
        RETURN Error(uri_error.Uri_Error("invalid_percent_encoding", 0))
    END IF
    RETURN Ok(native_percent_decode(value))
END FUNCTION

EXPORT FUNCTION query_encode(pairs)
    VAR output = ""
    VAR first = TRUE
    FOR pair IN pairs
        key = percent_encode(pair.name, "query_field")
        SELECT key
        CASE Error(problem)
            RETURN Error(problem)
        CASE Ok(encoded_key)
            value = percent_encode(pair.value, "query_field")
            SELECT value
            CASE Error(problem)
                RETURN Error(problem)
            CASE Ok(encoded_value)
                IF NOT first THEN
                    output = output + "&"
                END IF
                output = output + encoded_key + "=" + encoded_value
                first = FALSE
            END SELECT
        END SELECT
    END FOR
    RETURN Ok(output)
END FUNCTION

EXPORT FUNCTION query_decode(value)
    VAR output = []
    IF value = "" THEN
        RETURN Ok(output)
    END IF
    parts = text.split(value, "&")
    VAR offset = 0
    FOR part IN parts
        separator = text.find(part, "=")
        VAR encoded_key = ""
        VAR encoded_value = ""
        IF separator < 0 THEN
            encoded_key = part
            encoded_value = ""
        ELSE
            encoded_key = text.slice(part, 0, separator)
            encoded_value = text.slice(part, separator + 1,
                text.length(part) - separator - 1)
        END IF
        key = percent_decode(encoded_key)
        SELECT key
        CASE Error(problem)
            RETURN Error(uri_error.Uri_Error(problem.code, offset + problem.byte_offset))
        CASE Ok(decoded_key)
            decoded = percent_decode(encoded_value)
            SELECT decoded
            CASE Error(problem)
                RETURN Error(uri_error.Uri_Error(problem.code, offset + separator + 1
                    + problem.byte_offset))
            CASE Ok(decoded_value)
                output = output + [uri_model.Query_Pair(decoded_key, decoded_value)]
            END SELECT
        END SELECT
        offset = offset + text.length(part) + 1
    END FOR
    RETURN Ok(output)
END FUNCTION
