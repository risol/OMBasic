IMPORT "@std/collections/list.bas"
IMPORT "@std/encoding.bas"
IMPORT "@std/network/tcp.bas"
IMPORT "@std/text/text.bas"
IMPORT "@std/uri/uri.bas"
IMPORT "./errors.bas"
IMPORT "@std/http/http.bas"
IMPORT "@std/http/header_fields.bas"

FUNCTION target_for(path, query_present, query)
    VAR target = path
    IF target = "" THEN
        target = "/"
    END IF
    IF query_present THEN
        RETURN target + "?" + query
    END IF
    RETURN target
END FUNCTION

FUNCTION lower_ascii(value)
    upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    lower = "abcdefghijklmnopqrstuvwxyz"
    VAR output = ""
    VAR index = 0
    WHILE index < text.length(value)
        character = text.slice(value, index, 1)
        position = text.find(upper, character)
        IF position >= 0 THEN
            output = output + text.slice(lower, position, 1)
        ELSE
            output = output + character
        END IF
        index = index + 1
    END WHILE
    RETURN output
END FUNCTION

FUNCTION port_number(value)
    IF value = "" THEN
        RETURN -1
    END IF
    VAR total = 0
    FOR character IN text.split(value, "")
        digit = text.find("0123456789", character)
        IF digit < 0 OR total > 6553 OR (total = 6553 AND digit > 5) THEN
            RETURN -1
        END IF
        total = total * 10 + digit
    END FOR
    RETURN total
END FUNCTION

FUNCTION response_closes_connection(headers)
    FOR line IN headers
        colon = text.find(line, ":")
        IF colon > 0 AND header_fields.name_equals(text.slice(line, 0, colon), "connection") THEN
            connection_value = text.slice(line, colon + 1, text.length(line) - colon - 1)
            FOR item IN text.split(connection_value, ",")
                IF lower_ascii(text.trim(item)) = "close" THEN
                    RETURN TRUE
                END IF
            END FOR
        END IF
    END FOR
    RETURN FALSE
END FUNCTION

FUNCTION parse_response_bytes(input_buffer, eof_received, head_request)
    IF head_request THEN
        IF eof_received THEN
            RETURN http.parse_head_eof(input_buffer)
        END IF
        RETURN http.parse_head(input_buffer)
    END IF
    IF eof_received THEN
        RETURN http.parse_eof(input_buffer)
    END IF
    RETURN http.parse(input_buffer)
END FUNCTION

EXPORT MESSAGE Http_Get(method, uri_text, headers_text, body, reply_id)
EXPORT MESSAGE Http_Completed(success, status, reason, headers, body, error_code, error_offset)
EXPORT MESSAGE Http_Close()

' One request is active at a time. Network work remains in the Network Actor.
EXPORT ACTOR Http_Client
    VAR busy = FALSE
    VAR active_reply = Actor_Id()
    VAR active_method = ""
    VAR active_headers_text = ""
    VAR active_body = encoding.Buffer([])
    VAR active_scheme = ""
    VAR active_host = ""
    VAR active_network_host = ""
    VAR active_port = 0
    VAR active_port_present = FALSE
    VAR active_path = ""
    VAR active_query_present = FALSE
    VAR active_query = ""
    VAR active_connection = 0
    VAR active_secure = FALSE
    VAR idle_connection = 0
    VAR idle_scheme = ""
    VAR idle_host = ""
    VAR idle_port = 0
    VAR idle_secure = FALSE
    VAR active_request_bytes = []
    VAR active_request_offset = 0
    VAR active_response_bytes = []
    VAR closing = FALSE

    RECEIVE Http_Close()
        IF NOT busy THEN
            IF idle_connection <> 0 THEN
                closing = TRUE
                IF idle_secure THEN
                    SEND Network, Tls_Close(idle_connection, SELF)
                ELSE
                    SEND Network, Tcp_Close(idle_connection, SELF)
                END IF
                idle_connection = 0
            ELSE
                STOP
            END IF
        END IF
    END RECEIVE

    RECEIVE Http_Get(method, uri_text, headers_text, body, reply_id)
        IF busy THEN
            SEND reply_id, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), "resource_exhausted", 0)
        ELSE
            busy = TRUE
            active_reply = reply_id
            active_method = method
            active_headers_text = headers_text
            active_body = body
            parsed = uri.parse(uri_text)
            SELECT parsed
            CASE Error(problem)
                SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem.code, problem.byte_offset)
                busy = FALSE
            CASE Ok(value)
                IF NOT value.scheme_present OR NOT value.authority_present OR value.user_info_present OR value.host = "" THEN
                    SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), "invalid_uri", 0)
                    busy = FALSE
                ELSE IF value.scheme <> "http" AND value.scheme <> "https" THEN
                    SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), "unsupported_scheme", 0)
                    busy = FALSE
                ELSE
                    active_scheme = lower_ascii(value.scheme)
                    active_host = value.host
                    active_network_host = active_host
                    IF text.length(active_host) >= 2 AND text.slice(active_host, 0, 1) = "[" AND text.slice(active_host, text.length(active_host) - 1, 1) = "]" THEN
                        active_network_host = text.slice(active_host, 1, text.length(active_host) - 2)
                    END IF
                    active_port_present = value.port_present
                    IF value.port_present THEN
                        active_port = port_number(value.port)
                    ELSE
                        active_port = uri.default_port(active_scheme)
                    END IF
                    IF active_port < 0 OR active_port > 65535 THEN
                        SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), "invalid_uri", 0)
                        busy = FALSE
                    ELSE
                    active_path = value.path
                    active_query_present = value.query_present
                    active_query = value.query
                    active_secure = active_scheme = "https"
                    active_connection = 0
                    active_response_bytes = []
                    active_request_offset = 0
                    IF idle_connection <> 0 AND idle_scheme = active_scheme AND idle_host = lower_ascii(active_host) AND idle_port = active_port AND idle_secure = active_secure THEN
                        active_connection = idle_connection
                        idle_connection = 0
                        target = target_for(active_path, active_query_present, active_query)
                        serialized = http.serialize_keepalive(active_method, active_scheme, active_host, FALSE, active_port_present, active_port, target, active_headers_text, active_body)
                        SELECT serialized
                        CASE Error(problem)
                            IF active_secure THEN
                                SEND Network, Tls_Close(active_connection, SELF)
                            ELSE
                                SEND Network, Tcp_Close(active_connection, SELF)
                            END IF
                            SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem.code, problem.byte_offset)
                            busy = FALSE
                        CASE Ok(request_value)
                            active_request_bytes = request_value.bytes
                            active_request_offset = 0
                            IF active_secure THEN
                                SEND Network, Tls_Send(active_connection, active_request_bytes, SELF)
                            ELSE
                                SEND Network, Tcp_Send(active_connection, active_request_bytes, SELF)
                            END IF
                        END SELECT
                    ELSE
                        IF idle_connection <> 0 THEN
                            IF idle_secure THEN
                                SEND Network, Tls_Close(idle_connection, SELF)
                            ELSE
                                SEND Network, Tcp_Close(idle_connection, SELF)
                            END IF
                            idle_connection = 0
                        END IF
                        SEND Network, Dns_Resolve(active_network_host, active_port, SELF)
                    END IF
                    END IF
                END IF
            END SELECT
        END IF
    END RECEIVE

    RECEIVE Dns_Resolve_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem, 0)
            busy = FALSE
        CASE Ok(addresses)
            IF list.length(addresses) = 0 THEN
                SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), "not_found", 0)
                busy = FALSE
            ELSE
                SEND Network, Tcp_Connect(addresses[0], active_port, SELF)
            END IF
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Connect_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem, 0)
            busy = FALSE
        CASE Ok(connection_id)
            active_connection = connection_id
            IF active_secure THEN
                SEND Network, Tls_Client_Start(active_connection, active_network_host, SELF)
            ELSE
                target = target_for(active_path, active_query_present, active_query)
                serialized = http.serialize_keepalive(active_method, active_scheme, active_host, FALSE, active_port_present, active_port, target, active_headers_text, active_body)
                SELECT serialized
                CASE Error(problem)
                    SEND Network, Tcp_Close(active_connection, SELF)
                    SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem.code, problem.byte_offset)
                    busy = FALSE
                CASE Ok(value)
                    active_request_bytes = value.bytes
                    active_request_offset = 0
                    SEND Network, Tcp_Send(active_connection, list.drop(active_request_bytes, active_request_offset), SELF)
                END SELECT
            END IF
        END SELECT
    END RECEIVE

    RECEIVE Tls_Client_Start_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            SEND Network, Tcp_Close(active_connection, SELF)
            SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem, 0)
            busy = FALSE
        CASE Ok(connection_id)
            active_connection = connection_id
            target = target_for(active_path, active_query_present, active_query)
            serialized = http.serialize_keepalive(active_method, active_scheme, active_host, FALSE, active_port_present, active_port, target, active_headers_text, active_body)
            SELECT serialized
            CASE Error(problem)
                SEND Network, Tls_Close(active_connection, SELF)
                SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem.code, problem.byte_offset)
                busy = FALSE
            CASE Ok(value)
                active_request_bytes = value.bytes
                active_request_offset = 0
                SEND Network, Tls_Send(active_connection, list.drop(active_request_bytes, active_request_offset), SELF)
            END SELECT
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Send_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            SEND Network, Tcp_Close(active_connection, SELF)
            SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem, 0)
            busy = FALSE
        CASE Ok(bytes_sent)
            IF bytes_sent <= 0 OR bytes_sent > list.length(active_request_bytes) - active_request_offset THEN
                SEND Network, Tcp_Close(active_connection, SELF)
                SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), "io_error", 0)
                busy = FALSE
            ELSE
                active_request_offset = active_request_offset + bytes_sent
                IF active_request_offset < list.length(active_request_bytes) THEN
                    SEND Network, Tcp_Send(active_connection, list.drop(active_request_bytes, active_request_offset), SELF)
                ELSE
                    SEND Network, Tcp_Receive(active_connection, 65536, SELF)
                END IF
            END IF
        END SELECT
    END RECEIVE

    RECEIVE Tls_Send_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            SEND Network, Tls_Close(active_connection, SELF)
            SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem, 0)
            busy = FALSE
        CASE Ok(bytes_sent)
            IF bytes_sent <= 0 OR bytes_sent > list.length(active_request_bytes) - active_request_offset THEN
                SEND Network, Tls_Close(active_connection, SELF)
                SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), "io_error", 0)
                busy = FALSE
            ELSE
                active_request_offset = active_request_offset + bytes_sent
                IF active_request_offset < list.length(active_request_bytes) THEN
                    SEND Network, Tls_Send(active_connection, list.drop(active_request_bytes, active_request_offset), SELF)
                ELSE
                    SEND Network, Tls_Receive(active_connection, 65536, SELF)
                END IF
            END IF
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Receive_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            IF problem = "end_of_stream" THEN
                parsed = parse_response_bytes(encoding.Buffer(active_response_bytes), TRUE, active_method = "HEAD")
                SELECT parsed
                CASE Ok(value)
                    SEND Network, Tcp_Close(active_connection, SELF)
                    SEND active_reply, Http_Completed(TRUE, value.status, value.reason, value.headers, value.body, "", 0)
                CASE Error(parse_problem)
                    SEND Network, Tcp_Close(active_connection, SELF)
                    SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), parse_problem.code, parse_problem.byte_offset)
                END SELECT
            ELSE
                SEND Network, Tcp_Close(active_connection, SELF)
                SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem, 0)
            END IF
            busy = FALSE
        CASE Ok(values)
            active_response_bytes = active_response_bytes + values
            parsed = parse_response_bytes(encoding.Buffer(active_response_bytes), FALSE, active_method = "HEAD")
            SELECT parsed
            CASE Ok(value)
                IF value.status < 200 OR response_closes_connection(value.headers) THEN
                    SEND Network, Tcp_Close(active_connection, SELF)
                ELSE
                    idle_connection = active_connection
                    idle_scheme = active_scheme
                    idle_host = lower_ascii(active_host)
                    idle_port = active_port
                    idle_secure = active_secure
                    active_connection = 0
                END IF
                SEND active_reply, Http_Completed(TRUE, value.status, value.reason, value.headers, value.body, "", 0)
                busy = FALSE
            CASE Error(problem)
                IF problem.code = "incomplete_response" THEN
                    SEND Network, Tcp_Receive(active_connection, 65536, SELF)
                ELSE
                    SEND Network, Tcp_Close(active_connection, SELF)
                    SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem.code, problem.byte_offset)
                    busy = FALSE
                END IF
            END SELECT
        END SELECT
    END RECEIVE

    RECEIVE Tls_Receive_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            IF problem = "end_of_stream" THEN
                parsed = parse_response_bytes(encoding.Buffer(active_response_bytes), TRUE, active_method = "HEAD")
                SELECT parsed
                CASE Ok(value)
                    SEND Network, Tls_Close(active_connection, SELF)
                    SEND active_reply, Http_Completed(TRUE, value.status, value.reason, value.headers, value.body, "", 0)
                CASE Error(parse_problem)
                    SEND Network, Tls_Close(active_connection, SELF)
                    SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), parse_problem.code, parse_problem.byte_offset)
                END SELECT
            ELSE
                SEND Network, Tls_Close(active_connection, SELF)
                SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem, 0)
            END IF
            busy = FALSE
        CASE Ok(values)
            active_response_bytes = active_response_bytes + values
            parsed = parse_response_bytes(encoding.Buffer(active_response_bytes), FALSE, active_method = "HEAD")
            SELECT parsed
            CASE Ok(value)
                IF value.status < 200 OR response_closes_connection(value.headers) THEN
                    SEND Network, Tls_Close(active_connection, SELF)
                ELSE
                    idle_connection = active_connection
                    idle_scheme = active_scheme
                    idle_host = lower_ascii(active_host)
                    idle_port = active_port
                    idle_secure = active_secure
                    active_connection = 0
                END IF
                SEND active_reply, Http_Completed(TRUE, value.status, value.reason, value.headers, value.body, "", 0)
                busy = FALSE
            CASE Error(problem)
                IF problem.code = "incomplete_response" THEN
                    SEND Network, Tls_Receive(active_connection, 65536, SELF)
                ELSE
                    SEND Network, Tls_Close(active_connection, SELF)
                    SEND active_reply, Http_Completed(FALSE, 0, "", [], encoding.Buffer([]), problem.code, problem.byte_offset)
                    busy = FALSE
                END IF
            END SELECT
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Close_Completed(outcome)
        IF closing THEN
            STOP
        END IF
    END RECEIVE

    RECEIVE Tls_Close_Completed(outcome)
        IF closing THEN
            STOP
        END IF
    END RECEIVE

END ACTOR
