IMPORT "@std/encoding.bas"

EXPORT FUNCTION payload_bytes(payload)
    RETURN payload.bytes
END FUNCTION

EXPORT FUNCTION buffer_from_received_bytes(values)
    RETURN encoding.Buffer(values)
END FUNCTION
