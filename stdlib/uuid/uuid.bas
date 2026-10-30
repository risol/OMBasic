IMPORT "@std/encoding.bas"
IMPORT "@std/text/text.bas"
IMPORT "@std/collections/list.bas"

EXPORT RECORD Uuid_Error(code)

EXPORT FUNCTION parse(value)
    IF text.length(value) <> 36 OR text.byte_length(value) <> 36 THEN
        RETURN Error(Uuid_Error("invalid_format"))
    END IF
    parts = text.split(value, "-")
    IF list.length(parts) <> 5 OR text.length(parts[0]) <> 8 OR text.length(parts[1]) <> 4 OR text.length(parts[2]) <> 4 OR text.length(parts[3]) <> 4 OR text.length(parts[4]) <> 12 THEN
        RETURN Error(Uuid_Error("invalid_format"))
    END IF
    hex_text = parts[0] + parts[1] + parts[2] + parts[3] + parts[4]
    decoded = encoding.hex_decode(hex_text)
    VAR bytes = [0]
    SELECT decoded
    CASE Ok(buffer)
        bytes = buffer.bytes
    CASE Error(problem)
        RETURN Error(Uuid_Error("invalid_format"))
    END SELECT
    RETURN Ok(encoding.Buffer(bytes))
END FUNCTION

EXPORT FUNCTION format(value)
    length_result = encoding.buffer_length(value)
    SELECT length_result
    CASE Error(problem)
        RETURN Error(Uuid_Error("invalid_buffer"))
    CASE Ok(length)
        IF length <> 16 THEN
            RETURN Error(Uuid_Error("invalid_length"))
        END IF
    END SELECT

    alphabet = "0123456789abcdef"
    VAR formatted = ""
    VAR index = 0
    FOR byte_value IN value.bytes
        formatted = formatted + text.slice(alphabet, byte_value / 16, 1)
        formatted = formatted + text.slice(alphabet, byte_value % 16, 1)
        IF index = 3 OR index = 5 OR index = 7 OR index = 9 THEN
            formatted = formatted + "-"
        END IF
        index = index + 1
    END FOR
    RETURN Ok(formatted)
END FUNCTION

EXPORT FUNCTION v4_from_random_bytes(value)
    length_result = encoding.buffer_length(value)
    SELECT length_result
    CASE Error(problem)
        RETURN Error(Uuid_Error("invalid_buffer"))
    CASE Ok(length)
        IF length <> 16 THEN
            RETURN Error(Uuid_Error("invalid_length"))
        END IF
    END SELECT

    alphabet = "0123456789abcdef"
    VAR formatted = ""
    VAR index = 0
    FOR byte_value IN value.bytes
        VAR adjusted = byte_value + 0
        IF index = 6 THEN
            adjusted = (byte_value % 16) + 64
        ELSE IF index = 8 THEN
            adjusted = (byte_value % 64) + 128
        END IF
        formatted = formatted + text.slice(alphabet, adjusted / 16, 1)
        formatted = formatted + text.slice(alphabet, adjusted % 16, 1)
        IF index = 3 OR index = 5 OR index = 7 OR index = 9 THEN
            formatted = formatted + "-"
        END IF
        index = index + 1
    END FOR
    RETURN Ok(formatted)
END FUNCTION
