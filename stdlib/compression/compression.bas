IMPORT "@std/collections/list.bas"
IMPORT "@std/encoding.bas"
IMPORT "@std/text/text.bas"
NATIVE IMPORT "./compression_native.omi"

EXPORT RECORD Compression_Error(code)

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
    alphabet = "0123456789abcdef"
    FOR byte_value IN value.bytes
        output = output + text.slice(alphabet, byte_value / 16, 1)
        output = output + text.slice(alphabet, byte_value % 16, 1)
    END FOR
    RETURN output
END FUNCTION

FUNCTION hex_buffer(value)
    VAR bytes = []
    VAR index = 0
    alphabet = "0123456789abcdef"
    WHILE index < text.length(value)
        high = text.find(alphabet, text.slice(value, index, 1))
        low = text.find(alphabet, text.slice(value, index + 1, 1))
        IF high < 0 OR low < 0 THEN
            RETURN encoding.Buffer([])
        END IF
        bytes = bytes + [high * 16 + low]
        index = index + 2
    END WHILE
    RETURN encoding.Buffer(bytes)
END FUNCTION

FUNCTION transform(value, format, decompress)
    IF NOT valid_buffer(value) THEN
        RETURN Error(Compression_Error("invalid_buffer"))
    END IF
    hex = buffer_hex(value)
    status = compression_status(hex, format, decompress)
    IF status = 1 THEN
        RETURN Error(Compression_Error("invalid_format"))
    ELSE IF status = 2 THEN
        RETURN Error(Compression_Error("checksum_mismatch"))
    ELSE IF status = 3 THEN
        RETURN Error(Compression_Error("resource_exhausted"))
    ELSE IF status = 4 THEN
        RETURN Error(Compression_Error("invalid_buffer"))
    ELSE IF status = 5 THEN
        RETURN Error(Compression_Error("unsupported"))
    ELSE IF status <> 0 THEN
        RETURN Error(Compression_Error("invalid_format"))
    END IF
    RETURN Ok(hex_buffer(native_transform(hex, format, decompress)))
END FUNCTION

EXPORT FUNCTION gzip_compress(value)
    RETURN transform(value, 0, 0)
END FUNCTION

EXPORT FUNCTION gzip_decompress(value)
    RETURN transform(value, 0, 1)
END FUNCTION

EXPORT FUNCTION deflate_compress(value)
    RETURN transform(value, 1, 0)
END FUNCTION

EXPORT FUNCTION deflate_decompress(value)
    RETURN transform(value, 1, 1)
END FUNCTION
