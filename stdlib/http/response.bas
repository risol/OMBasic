IMPORT "@std/collections/list.bas"
IMPORT "@std/encoding.bas"
IMPORT "@std/text/text.bas"
IMPORT "./header_fields.bas"
IMPORT "./errors.bas"

EXPORT RECORD Http_Response(status, reason, headers, body)

FUNCTION quotient(value, divisor)
    VAR remainder = value
    VAR quotient_value = 0
    WHILE remainder >= divisor
        remainder = remainder - divisor
        quotient_value = quotient_value + 1
    END WHILE
    RETURN quotient_value
END FUNCTION

FUNCTION parse_unsigned(value, radix, maximum)
    IF value = "" THEN
        RETURN -1
    END IF
    VAR total = 0
    VAR index = 0
    alphabet = "0123456789abcdef"
    WHILE index < text.length(value)
        character = text.slice(value, index, 1)
        VAR digit = text.find(alphabet, character)
        IF digit < 0 THEN
            VAR upper_digit = text.find("ABCDEF", character)
            IF upper_digit >= 0 THEN
                digit = upper_digit + 10
            END IF
        END IF
        IF digit < 0 OR digit >= radix OR total > quotient(maximum - digit, radix) THEN
            RETURN -1
        END IF
        total = total * radix + digit
        index = index + 1
    END WHILE
    RETURN total
END FUNCTION

FUNCTION find_crlf(bytes, start)
    VAR index = start
    WHILE index + 1 < list.length(bytes)
        IF bytes[index] = 13 AND bytes[index + 1] = 10 THEN
            RETURN index
        END IF
        index = index + 1
    END WHILE
    RETURN -1
END FUNCTION

FUNCTION ascii_line(bytes, start, finish)
    VAR line_bytes = encoding.Buffer([]).bytes
    VAR index = start
    WHILE index < finish
        byte_value = bytes[index]
        IF byte_value > 127 OR byte_value = 0 THEN
            RETURN ""
        END IF
        line_bytes = line_bytes + [byte_value]
        index = index + 1
    END WHILE
    alphabet = "0123456789abcdef"
    VAR hex = ""
    FOR byte_value IN line_bytes
        hex = hex + text.slice(alphabet, quotient(byte_value, 16), 1)
        hex = hex + text.slice(alphabet, byte_value % 16, 1)
    END FOR
    RETURN encoding.utf8_decode_hex_raw(hex)
END FUNCTION

FUNCTION chunked_body(bytes, start)
    VAR body = encoding.Buffer([]).bytes
    VAR cursor = start
    WHILE TRUE
        line_end = find_crlf(bytes, cursor)
        IF line_end < 0 OR line_end - cursor > 128 THEN
            RETURN Error(errors.Http_Problem("malformed_response", cursor))
        END IF
        size_text = ascii_line(bytes, cursor, line_end)
        IF size_text = "" THEN
            RETURN Error(errors.Http_Problem("malformed_response", cursor))
        END IF
            VAR chunk_size_text = size_text
            extension_start = text.find(chunk_size_text, ";")
            IF extension_start >= 0 THEN
                chunk_size_text = text.slice(chunk_size_text, 0, extension_start)
            END IF
            chunk_size = parse_unsigned(chunk_size_text, 16, 67108864)
            IF chunk_size < 0 THEN
                RETURN Error(errors.Http_Problem("malformed_response", cursor))
            END IF
                cursor = line_end + 2
                IF chunk_size < 0 OR chunk_size > 67108864 - list.length(body) THEN
                    RETURN Error(errors.Http_Problem("resource_exhausted", cursor))
                END IF
                IF chunk_size = 0 THEN
                    WHILE TRUE
                        trailer_end = find_crlf(bytes, cursor)
                        IF trailer_end < 0 THEN
                            RETURN Error(errors.Http_Problem("incomplete_response", cursor))
                        END IF
                        IF trailer_end = cursor THEN
                            cursor = cursor + 2
                            IF cursor <> list.length(bytes) THEN
                                RETURN Error(errors.Http_Problem("malformed_response", cursor))
                            END IF
                            RETURN Ok(body)
                        END IF
                        trailer_line = ascii_line(bytes, cursor, trailer_end)
                        IF trailer_line = "" OR NOT header_fields.line_valid(trailer_line) THEN
                            RETURN Error(errors.Http_Problem("malformed_response", cursor))
                        END IF
                        cursor = trailer_end + 2
                    END WHILE
                END IF
                IF cursor + chunk_size + 2 > list.length(bytes) THEN
                    RETURN Error(errors.Http_Problem("incomplete_response", cursor))
                END IF
                VAR index = 0
                WHILE index < chunk_size
                    body = body + [bytes[cursor + index]]
                    index = index + 1
                END WHILE
                cursor = cursor + chunk_size
                IF bytes[cursor] <> 13 OR bytes[cursor + 1] <> 10 THEN
                    RETURN Error(errors.Http_Problem("malformed_response", cursor))
                END IF
                cursor = cursor + 2
    END WHILE
    RETURN Error(errors.Http_Problem("malformed_response", cursor))
END FUNCTION

FUNCTION parse_internal(input_buffer, eof_received, head_response)
    bytes = input_buffer.bytes
    IF list.length(bytes) > 67108864 THEN
        RETURN Error(errors.Http_Problem("resource_exhausted", 0))
    END IF
    FOR byte_value IN bytes
        IF byte_value < 0 OR byte_value > 255 THEN
            RETURN Error(errors.Http_Problem("invalid_buffer", 0))
        END IF
    END FOR
    status_end = find_crlf(bytes, 0)
    IF status_end < 0 OR status_end > 8192 THEN
        RETURN Error(errors.Http_Problem("incomplete_response", 0))
    END IF
    status_text = ascii_line(bytes, 0, status_end)
    IF status_text = "" THEN
        RETURN Error(errors.Http_Problem("malformed_response", 0))
    END IF
        status_parts = text.split(status_text, " ")
        IF list.length(status_parts) < 2 OR NOT text.starts_with(status_parts[0], "HTTP/1.") THEN
            RETURN Error(errors.Http_Problem("malformed_response", 0))
        END IF
        status = parse_unsigned(status_parts[1], 10, 599)
        IF status < 100 THEN
            RETURN Error(errors.Http_Problem("malformed_response", 0))
        END IF
            IF status < 100 OR status > 599 THEN
                RETURN Error(errors.Http_Problem("malformed_response", 0))
            END IF
            VAR reason = ""
            IF list.length(status_parts) > 2 THEN
                reason = text.join(list.drop(status_parts, 2), " ")
            END IF
            VAR headers = text.split("", "")
            VAR cursor = status_end + 2
            VAR total_header_bytes = 0
            VAR header_end = -1
            WHILE cursor <= list.length(bytes)
                line_end = find_crlf(bytes, cursor)
                IF line_end < 0 THEN
                    RETURN Error(errors.Http_Problem("incomplete_response", cursor))
                END IF
                total_header_bytes = total_header_bytes + line_end - cursor + 2
                IF total_header_bytes > 65536 OR list.length(headers) >= 256 THEN
                    RETURN Error(errors.Http_Problem("resource_exhausted", cursor))
                END IF
                IF cursor = line_end THEN
                    header_end = cursor + 2
                    BREAK
                END IF
                line = ascii_line(bytes, cursor, line_end)
                IF line = "" OR NOT header_fields.line_valid(line) THEN
                    RETURN Error(errors.Http_Problem("malformed_response", cursor))
                END IF
                headers = headers + [text.trim(line)]
                cursor = line_end + 2
            END WHILE
            IF header_end < 0 THEN
                RETURN Error(errors.Http_Problem("incomplete_response", cursor))
            END IF
            VAR body = encoding.Buffer([]).bytes
            content_length_count = header_fields.count(headers, "content-length")
            transfer_encoding_count = header_fields.count(headers, "transfer-encoding")
            IF content_length_count > 1 OR transfer_encoding_count > 1 OR (content_length_count > 0 AND transfer_encoding_count > 0) THEN
                RETURN Error(errors.Http_Problem("malformed_response", header_end))
            END IF
            IF head_response THEN
                IF list.length(bytes) <> header_end THEN
                    RETURN Error(errors.Http_Problem("malformed_response", header_end))
                END IF
                body = []
            ELSE IF status < 200 OR status = 204 OR status = 304 THEN
                body = []
            ELSE IF transfer_encoding_count > 0 THEN
                transfer_encoding = header_fields.value_for(headers, "transfer-encoding")
                IF NOT header_fields.name_equals(text.trim(transfer_encoding), "chunked") THEN
                    RETURN Error(errors.Http_Problem("unsupported_transfer_encoding", header_end))
                END IF
                decoded_body = chunked_body(bytes, header_end)
                SELECT decoded_body
                CASE Error(problem)
                    RETURN Error(problem)
                CASE Ok(value)
                    body = value
                END SELECT
            ELSE IF content_length_count > 0 THEN
                length_text = header_fields.value_for(headers, "content-length")
                body_length = parse_unsigned(text.trim(length_text), 10, 67108864)
                IF body_length < 0 THEN
                    RETURN Error(errors.Http_Problem("malformed_response", header_end))
                END IF
                    available = list.length(bytes) - header_end
                    IF body_length < 0 OR body_length > 67108864 THEN
                        RETURN Error(errors.Http_Problem("resource_exhausted", header_end))
                    END IF
                    IF available < body_length THEN
                        RETURN Error(errors.Http_Problem("incomplete_response", list.length(bytes)))
                    END IF
                    IF available <> body_length THEN
                        RETURN Error(errors.Http_Problem("malformed_response", header_end + body_length))
                    END IF
                    body = list.slice(bytes, header_end, body_length)
            ELSE
                IF NOT eof_received THEN
                    RETURN Error(errors.Http_Problem("incomplete_response", list.length(bytes)))
                END IF
                body = list.drop(bytes, header_end)
            END IF
            IF list.length(body) > 67108864 THEN
                RETURN Error(errors.Http_Problem("resource_exhausted", header_end))
            END IF
            RETURN Ok(Http_Response(status, reason, headers, encoding.Buffer(body)))
END FUNCTION

EXPORT FUNCTION parse(input_buffer)
    RETURN parse_internal(input_buffer, FALSE, FALSE)
END FUNCTION

EXPORT FUNCTION parse_eof(input_buffer)
    RETURN parse_internal(input_buffer, TRUE, FALSE)
END FUNCTION

EXPORT FUNCTION parse_head(input_buffer)
    RETURN parse_internal(input_buffer, FALSE, TRUE)
END FUNCTION

EXPORT FUNCTION parse_head_eof(input_buffer)
    RETURN parse_internal(input_buffer, TRUE, TRUE)
END FUNCTION


