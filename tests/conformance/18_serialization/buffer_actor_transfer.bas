IMPORT "@std/encoding.bas"

MESSAGE Buffer_Payload(buffer)

ACTOR Buffer_Sink
    RECEIVE Buffer_Payload(buffer)
        print($"bytes={buffer.bytes[0]}|{buffer.bytes[1]}|{buffer.bytes[2]}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    created = encoding.buffer_from_bytes([0, 127, 255])
    SELECT created
    CASE Ok(value)
        sink = SPAWN Buffer_Sink
        SEND sink, Buffer_Payload(value)
    CASE Error(problem)
        print($"buffer-error={problem.code}")
    END SELECT
END ACTION
