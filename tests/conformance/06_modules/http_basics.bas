IMPORT "@std/http/http.bas"
IMPORT "@std/encoding.bas"
IMPORT "@std/uri/uri.bas"
IMPORT "@std/collections/list.bas"
IMPORT "@std/text/text.bas"

ACTION main()
    parsed_uri = uri.parse("http://example.com/a?b=1")
    SELECT parsed_uri
    CASE Error(uri_problem)
        print($"uri-error={uri_problem.code}")
    CASE Ok(target)
        wire = http.serialize("POST", "http", "example.com", FALSE, FALSE, 0, http.request_target(target.path, target.query_present, target.query), http.make_header("X-Test", "value"), encoding.Buffer([0, 255]))
        SELECT wire
        CASE Error(problem)
            print($"request-error={problem.code}")
        CASE Ok(bytes)
            head = encoding.utf8_decode(encoding.Buffer(
                list.take(bytes.bytes, list.length(bytes.bytes) - 2)))
            SELECT head
            CASE Error(problem)
                print($"head-error={problem.code}")
            CASE Ok(head_text)
                print($"request={text.ends_with(head_text, "Content-Length: 2\r\nConnection: close\r\nX-Test: value\r\n\r\n")}|body={bytes.bytes[list.length(bytes.bytes) - 2]}|{bytes.bytes[list.length(bytes.bytes) - 1]}")
            END SELECT
        END SELECT
        response = http.parse(encoding.Buffer([72, 84, 84, 80, 47, 49, 46, 49, 32, 50, 48, 48,
            32, 79, 75, 13, 10, 67, 111, 110, 116, 101, 110, 116, 45, 76, 101,
            110, 103, 116, 104, 58, 32, 51, 13, 10, 13, 10, 0, 65, 255]))
        SELECT response
        CASE Error(problem)
            print($"response-error={problem.code}|{problem.byte_offset}")
        CASE Ok(value)
            print($"response={value.status}|{value.reason}|{value.body.bytes[0]}|{value.body.bytes[2]}")
        END SELECT
        chunked = http.parse(encoding.Buffer([72, 84, 84, 80, 47, 49, 46, 49, 32, 50, 48, 48,
            32, 79, 75, 13, 10, 84, 114, 97, 110, 115, 102, 101, 114, 45, 69,
            110, 99, 111, 100, 105, 110, 103, 58, 32, 99, 104, 117, 110, 107, 101,
            100, 13, 10, 13, 10, 51, 13, 10, 97, 98, 99, 13, 10, 48, 13, 10,
            13, 10]))
        SELECT chunked
        CASE Error(problem)
            print($"chunked-error={problem.code}")
        CASE Ok(value)
            print($"chunked={value.status}|{value.body.bytes[0]}|{value.body.bytes[2]}")
        END SELECT
        close_delimited = encoding.Buffer([72, 84, 84, 80, 47, 49, 46, 49, 32, 50, 48, 48,
            32, 79, 75, 13, 10, 13, 10, 0, 255])
        before_eof = http.parse(close_delimited)
        SELECT before_eof
        CASE Error(problem)
            print($"before-eof={problem.code}")
        CASE Ok(value)
            print("before-eof=unexpected-success")
        END SELECT
        at_eof = http.parse_eof(close_delimited)
        SELECT at_eof
        CASE Error(problem)
            print($"at-eof-error={problem.code}")
        CASE Ok(value)
            print($"at-eof={value.status}|{value.body.bytes[0]}|{value.body.bytes[1]}")
        END SELECT
        head_response = http.parse_head(encoding.Buffer([72, 84, 84, 80, 47, 49, 46, 49, 32, 50, 48, 48, 32, 79, 75, 13, 10, 67, 111, 110, 116, 101, 110, 116, 45, 76, 101, 110, 103, 116, 104, 58, 32, 51, 13, 10, 13, 10]))
        SELECT head_response
        CASE Error(problem)
            print($"head-error={problem.code}")
        CASE Ok(value)
            print($"head={value.status}|{list.length(value.body.bytes)}")
        END SELECT
        head_with_body = http.parse_head(encoding.Buffer([72, 84, 84, 80, 47, 49, 46, 49, 32, 50, 48, 48, 32, 79, 75, 13, 10, 13, 10, 65]))
        SELECT head_with_body
        CASE Error(problem)
            print($"head-body={problem.code}")
        CASE Ok(unused_value)
            print("head-body=accepted")
        END SELECT
        bad_header = http.serialize("GET", "http", "example.com", FALSE, FALSE, 0, http.request_target(target.path, target.query_present, target.query), http.make_header("X-Test", "bad\r\nInjected: yes"), encoding.Buffer([]))
        SELECT bad_header
        CASE Error(problem)
            print($"injection={problem.code}")
        CASE Ok(unused_value)
            print("injection=accepted")
        END SELECT
        bad_target = http.serialize("GET", "http", "example.com", FALSE, FALSE, 0, "/safe\r\nInjected: yes", "", encoding.Buffer([]))
        SELECT bad_target
        CASE Error(problem)
            print($"target-injection={problem.code}")
        CASE Ok(unused_value)
            print("target-injection=accepted")
        END SELECT
        bad_host = http.serialize("GET", "http", "example.com\r\nInjected: yes", FALSE, FALSE, 0, "/", "", encoding.Buffer([]))
        SELECT bad_host
        CASE Error(problem)
            print($"host-injection={problem.code}")
        CASE Ok(unused_value)
            print("host-injection=accepted")
        END SELECT
    END SELECT
END ACTION



