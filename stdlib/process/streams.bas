IMPORT "@std/encoding.bas"

EXPORT FUNCTION bytes_for_stdin(value)
    RETURN value.bytes
END FUNCTION

EXPORT FUNCTION buffer_from_read_bytes(values)
    RETURN encoding.Buffer(values)
END FUNCTION
