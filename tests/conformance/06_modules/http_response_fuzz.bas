IMPORT "@std/collections/list.bas"
IMPORT "@std/encoding.bas"
IMPORT "@std/http/http.bas"

ACTION main()
    VAR seed = 1
    WHILE seed <= 128
        VAR state = seed
        VAR length = seed % 65
        VAR bytes = []
        VAR index = 0
        WHILE index < length
            state = (state * 48271) % 2147483647
            bytes = bytes + [state % 256]
            index = index + 1
        END WHILE
        input_buffer = encoding.Buffer(bytes)
        parsed = http.parse(input_buffer)
        parsed_eof = http.parse_eof(input_buffer)
        parsed_head = http.parse_head(input_buffer)
        parsed_head_eof = http.parse_head_eof(input_buffer)
        seed = seed + 1
    END WHILE
    print("http-parser-fuzz=128")
END ACTION
