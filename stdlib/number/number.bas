NATIVE IMPORT "./number_native.omi"

EXPORT RECORD Number_Error(code, byte_offset, detail)

FUNCTION status_code(status)
    IF status = 1 THEN
        RETURN "empty"
    END IF
    IF status = 2 THEN
        RETURN "invalid_digit"
    END IF
    IF status = 3 THEN
        RETURN "invalid_radix"
    END IF
    IF status = 4 THEN
        RETURN "out_of_range"
    END IF
    IF status = 5 THEN
        RETURN "non_finite"
    END IF
    RETURN "invalid_format"
END FUNCTION

EXPORT FUNCTION parse_int(text)
    RETURN parse_int_radix(text, 10)
END FUNCTION

EXPORT FUNCTION parse_int_radix(text, radix)
    status = int_status(text, radix)
    IF status = 0 THEN
        RETURN Ok(int_value(text, radix))
    END IF
    code = status_code(status)
    RETURN Error(Number_Error(code, int_offset(text, radix), code))
END FUNCTION

EXPORT FUNCTION format_int(value)
    RETURN int_format(value, 10)
END FUNCTION

EXPORT FUNCTION format_int_radix(value, radix)
    IF radix < 2 OR radix > 36 THEN
        RETURN Error(Number_Error("invalid_radix", 0, "invalid_radix"))
    END IF
    RETURN Ok(int_format(value, radix))
END FUNCTION

EXPORT FUNCTION parse_bool(text)
    IF text = "true" THEN
        RETURN Ok(TRUE)
    END IF
    IF text = "false" THEN
        RETURN Ok(FALSE)
    END IF
    RETURN Error(Number_Error("invalid_format", 0, "invalid_format"))
END FUNCTION

EXPORT FUNCTION format_bool(value)
    IF value THEN
        RETURN "true"
    END IF
    RETURN "false"
END FUNCTION

EXPORT FUNCTION parse_float(text)
    status = float_status(text)
    IF status = 0 THEN
        RETURN Ok(float_value(text))
    END IF
    code = status_code(status)
    RETURN Error(Number_Error(code, float_offset(text), code))
END FUNCTION

EXPORT FUNCTION format_float(value)
    IF NOT float_is_finite(value) THEN
        RETURN Error(Number_Error("non_finite", 0, "non_finite"))
    END IF
    RETURN Ok(float_format(value))
END FUNCTION
