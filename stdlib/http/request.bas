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
    VAR headers = QUERY source_line IN text.split(headers_text, "\n")
        WHERE text.trim(source_line) <> ""
        SELECT text.trim(source_line)
    END QUERY
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
    VAR request_lines = [method + " " + target + " HTTP/1.1", "Host: " + host_value]
    IF has_length = 0 THEN
        request_lines = request_lines + ["Content-Length: " + number.format_int(body_length)]
    END IF
    IF header_fields.count(headers, "connection") > 0 THEN
        RETURN Error(errors.Http_Problem("invalid_header", 0))
    END IF
    request_lines = request_lines + ["Connection: " + connection_value] + headers + ["", ""]
    request_text = text.join(request_lines, "\r\n")
    status = encoding.utf8_encode_status_code(request_text)
    IF status = 2 THEN
        RETURN Error(errors.Http_Problem("resource_exhausted", 0))
    END IF
    IF status <> 0 THEN
        RETURN Error(errors.Http_Problem("invalid_utf8", 0))
    END IF
    head = string_utf8_bytes(request_text)
    RETURN Ok(encoding.Buffer(head + body))
END FUNCTION

EXPORT FUNCTION serialize(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer)
    RETURN serialize_with_connection(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer, "close")
END FUNCTION

EXPORT FUNCTION serialize_keepalive(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer)
    RETURN serialize_with_connection(method, scheme, host, user_info_present, port_present, port, target, headers_text, body_buffer, "keep-alive")
END FUNCTION


