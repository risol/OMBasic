IMPORT "@std/encoding.bas"
IMPORT "@std/collections/list.bas"
IMPORT "@std/text/text.bas"
NATIVE IMPORT "./hash_native.omi"

EXPORT RECORD Hash_Error(code, byte_offset)

FUNCTION hex_digit(value)
    RETURN text.slice("0123456789abcdef", value, 1)
END FUNCTION

FUNCTION quotient(value, divisor)
    VAR remainder = value
    VAR quotient_value = 0
    WHILE remainder >= divisor
        remainder = remainder - divisor
        quotient_value = quotient_value + 1
    END WHILE
    RETURN quotient_value
END FUNCTION

FUNCTION valid_buffer(value)
    IF list.length(value.bytes) > 67108864 THEN
        RETURN FALSE
    END IF
    FOR byte_value IN value.bytes
        IF byte_value < 0 OR byte_value > 255 THEN
            RETURN FALSE
        END IF
    END FOR
    RETURN TRUE
END FUNCTION

FUNCTION buffer_hex(value)
    VAR output = ""
    FOR byte_value IN value.bytes
        output = output + hex_digit(quotient(byte_value, 16))
        output = output + hex_digit(byte_value % 16)
    END FOR
    RETURN output
END FUNCTION

EXPORT FUNCTION sha256_hex(value)
    IF NOT valid_buffer(value) THEN
        RETURN Error(Hash_Error("invalid_buffer", 0))
    END IF
    hex = buffer_hex(value)
    status = sha256_status(hex)
    IF status = 2 THEN
        RETURN Error(Hash_Error("resource_exhausted", 0))
    END IF
    IF status <> 0 THEN
        RETURN Error(Hash_Error("invalid_buffer", 0))
    END IF
    RETURN Ok(native_sha256_hex(hex))
END FUNCTION

EXPORT FUNCTION sha256(value)
    IF NOT valid_buffer(value) THEN
        RETURN encoding.Buffer([])
    END IF
    hex = native_sha256_hex(buffer_hex(value))
    VAR bytes = []
    VAR index = 0
    WHILE index < 64
        high = text.find("0123456789abcdef", text.slice(hex, index, 1))
        low = text.find("0123456789abcdef", text.slice(hex, index + 1, 1))
        bytes = bytes + [high * 16 + low]
        index = index + 2
    END WHILE
    RETURN encoding.Buffer(bytes)
END FUNCTION

EXPORT FUNCTION crc32(value)
    IF NOT valid_buffer(value) THEN
        RETURN Error(Hash_Error("invalid_buffer", 0))
    END IF
    hex = buffer_hex(value)
    status = crc32_status(hex)
    IF status = 2 THEN
        RETURN Error(Hash_Error("resource_exhausted", 0))
    END IF
    IF status <> 0 THEN
        RETURN Error(Hash_Error("invalid_buffer", 0))
    END IF
    RETURN Ok(native_crc32(hex))
END FUNCTION
