IMPORT "@std/collections/list.bas"
IMPORT "@std/encoding.bas"
IMPORT "@std/number/number.bas"
IMPORT "@std/text/text.bas"
IMPORT "./header_fields.bas"
IMPORT "./errors.bas"

FUNCTION serialize_with_connection(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer, connection_value)
    body = body_buffer.bytes
    IF text.find(headers_text, "\r") >= 0 THEN
        RETURN Error(errors.Http_Problem("invalid_header", 0))
    END IF
    VAR headers = list.take(text.split(headers_text, "\n"), 0)
    IF headers_text <> "" THEN
        FOR source_line IN text.split(headers_text, "\n")
            line = text.trim(source_line)
            IF line <> "" THEN
                headers = headers + [line]
            END IF
        END FOR
    END IF
    IF NOT header_fields.list_valid(headers) THEN
        RETURN Error(errors.Http_Problem("invalid_header", 0))
    END IF
    IF NOT header_fields.valid_name(method) THEN
        RETURN Error(errors.Http_Problem("invalid_method", 0))
    END IF
    IF host = "" OR user_info_present OR (scheme <> "http" AND scheme <> "https") THEN
        RETURN Error(errors.Http_Problem("invalid_uri", 0))
    END IF
    IF text.find(host, "\r") >= 0 OR text.find(host, "\n") >= 0 OR text.find(host, " ") >= 0 OR text.find(host, "\t") >= 0 THEN
        RETURN Error(errors.Http_Problem("invalid_uri", 0))
    END IF
    IF text.find(target, "\r") >= 0 OR text.find(target, "\n") >= 0 OR text.find(target, " ") >= 0 THEN
        RETURN Error(errors.Http_Problem("invalid_uri", 0))
    END IF
    IF port_present AND (port < 0 OR port > 65535) THEN
        RETURN Error(errors.Http_Problem("invalid_uri", 0))
    END IF
    body_length = list.length(body)
    IF body_length > 67108864 THEN
        RETURN Error(errors.Http_Problem("resource_exhausted", 0))
    END IF
    VAR host_value = host
    IF port_present THEN
        default_port = (scheme = "http" AND port = 80) OR (scheme = "https" AND port = 443)
        IF NOT default_port THEN
            host_value = host_value + ":" + number.format_int(port)
        END IF
    END IF
    VAR request_text = method + " " + target + " HTTP/1.1\r\nHost: " + host_value + "\r\n"
    IF header_fields.count(headers, "host") <> 0 THEN
        RETURN Error(errors.Http_Problem("invalid_header", 0))
    END IF
    has_length = header_fields.count(headers, "content-length")
    has_transfer = header_fields.count(headers, "transfer-encoding")
    IF has_length > 1 OR has_transfer > 0 THEN
        RETURN Error(errors.Http_Problem("invalid_header", 0))
    END IF
    IF has_length = 1 AND text.trim(header_fields.value_for(headers, "content-length")) <> number.format_int(body_length) THEN
        RETURN Error(errors.Http_Problem("invalid_header", 0))
    END IF
    IF has_length = 0 THEN
        request_text = request_text + "Content-Length: " + number.format_int(body_length) + "\r\n"
    END IF
    IF header_fields.count(headers, "connection") > 0 THEN
        RETURN Error(errors.Http_Problem("invalid_header", 0))
    END IF
    request_text = request_text + "Connection: " + connection_value + "\r\n"
    FOR line IN headers
        request_text = request_text + text.trim(line) + "\r\n"
    END FOR
    request_text = request_text + "\r\n"
    status = encoding.utf8_encode_status_code(request_text)
    IF status = 2 THEN
        RETURN Error(errors.Http_Problem("resource_exhausted", 0))
    END IF
    IF status <> 0 THEN
        RETURN Error(errors.Http_Problem("invalid_utf8", 0))
    END IF
    hex = encoding.utf8_encode_hex_raw(request_text)
    VAR head = []
    VAR index = 0
    alphabet = "0123456789abcdef"
    WHILE index < text.length(hex)
        high = text.find(alphabet, text.slice(hex, index, 1))
        low = text.find(alphabet, text.slice(hex, index + 1, 1))
        head = head + [high * 16 + low]
        index = index + 2
    END WHILE
    RETURN Ok(encoding.Buffer(head + body))
END FUNCTION

EXPORT FUNCTION serialize(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer)
    RETURN serialize_with_connection(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer, "close")
END FUNCTION

EXPORT FUNCTION serialize_keepalive(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer)
    RETURN serialize_with_connection(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer, "keep-alive")
END FUNCTION


