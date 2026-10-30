IMPORT "@std/logging/logging.bas"

ACTION main()
    fields = [logging.Log_Field("user", "ada"),
              logging.Log_Field("detail", "first\nsecond\tend\\")]
    event = logging.Log_Event(1234, "info", "server", "started\r\n\u{1}ready", fields)
    print(logging.format_text(event))
    empty = logging.Log_Event(0, "debug", "", "", [])
    print(logging.format_text(empty))
END ACTION
