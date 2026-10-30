IMPORT "@std/encoding.bas"

EXPORT FUNCTION buffer_from_read_bytes(values)
    RETURN encoding.buffer_from_bytes(values)
END FUNCTION

EXPORT FUNCTION bytes_for_write(value)
    checked = encoding.buffer_length(value)
    SELECT checked
    CASE Ok(length)
        RETURN Ok(value.bytes)
    CASE Error(problem)
        RETURN Error(problem)
    END SELECT
END FUNCTION
