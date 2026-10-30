IMPORT "@std/encoding.bas"
IMPORT "@std/process/streams.bas"

ACTION main()
    payload = encoding.Buffer([65, 66])
    values = streams.bytes_for_stdin(payload)
    restored = streams.buffer_from_read_bytes(values)
    print($"bytes={restored.bytes[0]},{restored.bytes[1]}")
END ACTION
