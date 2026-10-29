IMPORT "./json_error.bas"
IMPORT "./json_native.bas"
IMPORT "@std/text/text.bas"

EXPORT RECORD Value(encoded)
EXPORT RECORD Entry(key, value)

EXPORT FUNCTION null_value()
    RETURN Value("null")
END FUNCTION

EXPORT FUNCTION boolean(flag)
    IF flag THEN
        RETURN Value("true")
    END IF
    RETURN Value("false")
END FUNCTION

EXPORT FUNCTION number(decimal_text)
    IF json_native.is_valid(decimal_text) THEN
        IF json_native.kind_code(decimal_text) = 2 THEN
            RETURN Ok(Value(json_native.compact(decimal_text)))
        END IF
        RETURN Error(json_error.Parse_Error(6, 0, "expected a JSON number"))
    END IF
    RETURN Error(json_error.Parse_Error(json_native.error_code(decimal_text),
                                   json_native.error_offset(decimal_text),
                                   json_native.error_message(decimal_text)))
END FUNCTION

EXPORT FUNCTION string_value(input_text)
    RETURN Value(json_native.quote(input_text))
END FUNCTION

EXPORT FUNCTION array(values)
    VAR encoded_items = []
    FOR item IN values
        encoded_items = encoded_items + [item.encoded]
    END FOR
    source = "[" + text.join(encoded_items, ",") + "]"
    IF json_native.is_valid(source) THEN
        RETURN Ok(Value(json_native.compact(source)))
    END IF
    RETURN Error(json_error.Parse_Error(json_native.error_code(source),
                                   json_native.error_offset(source),
                                   json_native.error_message(source)))
END FUNCTION

EXPORT FUNCTION object(entries)
    VAR encoded_entries = []
    FOR current_entry IN entries
        encoded_key = json_native.quote(current_entry.key)
        encoded_entries = encoded_entries + [(encoded_key + ":") + current_entry.value.encoded]
    END FOR
    source = "{" + text.join(encoded_entries, ",") + "}"
    IF json_native.is_valid(source) THEN
        RETURN Ok(Value(json_native.compact(source)))
    END IF
    RETURN Error(json_error.Parse_Error(json_native.error_code(source),
                                   json_native.error_offset(source),
                                   json_native.error_message(source)))
END FUNCTION
