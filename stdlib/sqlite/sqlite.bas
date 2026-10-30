IMPORT "@std/collections/list.bas"
IMPORT "@std/number/number.bas"
IMPORT "@std/text/text.bas"
NATIVE IMPORT "./sqlite_native.omi"

EXPORT RECORD Database_Error(code, native_code)
EXPORT MESSAGE Database_Open(path, reply_id)
EXPORT MESSAGE Database_Opened(connection_id, error_code, native_code)
EXPORT MESSAGE Database_Execute(sql, reply_id)
EXPORT MESSAGE Database_Execute_Completed(success, changes, last_row_id, error_code, native_code)
EXPORT MESSAGE Database_Query(sql, reply_id)
EXPORT MESSAGE Database_Query_Completed(success, json_text, error_code, native_code)
EXPORT MESSAGE Database_Close(reply_id)
EXPORT MESSAGE Database_Closed(success, error_code, native_code)

FUNCTION parse_native_error(value)
    VAR parts = text.split(value, "|")
    IF list.length(parts) = 3 AND parts[0] = "error" THEN
        native_result = number.parse_int(parts[2])
        SELECT native_result
        CASE Ok(native_code)
            RETURN Error(Database_Error(parts[1], native_code))
        CASE Error(problem)
            RETURN Error(Database_Error("invalid_native_result", 0))
        END SELECT
    END IF
    RETURN Error(Database_Error("invalid_native_result", 0))
END FUNCTION

FUNCTION parse_open_result(result_text)
    VAR parts = text.split(result_text, "|")
    IF list.length(parts) = 2 AND parts[0] = "ok" THEN
        id_result = number.parse_int(parts[1])
        SELECT id_result
        CASE Ok(connection_id)
            RETURN Ok(connection_id)
        CASE Error(problem)
            RETURN Error(Database_Error("invalid_native_result", 0))
        END SELECT
    END IF
    RETURN parse_native_error(result_text)
END FUNCTION

FUNCTION parse_execute_result(result_text)
    VAR parts = text.split(result_text, "|")
    IF list.length(parts) = 3 AND parts[0] = "ok" THEN
        changes_result = number.parse_int(parts[1])
        row_id_result = number.parse_int(parts[2])
        SELECT changes_result
        CASE Ok(changes)
            SELECT row_id_result
            CASE Ok(row_id)
                RETURN Ok([changes, row_id])
            CASE Error(problem)
                RETURN Error(Database_Error("invalid_native_result", 0))
            END SELECT
        CASE Error(problem)
            RETURN Error(Database_Error("invalid_native_result", 0))
        END SELECT
    END IF
    RETURN parse_native_error(result_text)
END FUNCTION

FUNCTION parse_query_result(result_text)
    IF text.starts_with(result_text, "ok|") THEN
        RETURN Ok(text.slice(result_text, 3, text.length(result_text) - 3))
    END IF
    RETURN parse_native_error(result_text)
END FUNCTION

FUNCTION parse_close_result(result_text)
    IF result_text = "ok" THEN
        RETURN Ok(TRUE)
    END IF
    RETURN parse_native_error(result_text)
END FUNCTION

EXPORT ACTOR Database
    VAR connection_id = 0

    RECEIVE Database_Open(path, reply_id)
        IF connection_id <> 0 THEN
            SEND reply_id, Database_Opened(connection_id, "already_open", 0)
        ELSE
            opened = parse_open_result(sqlite_native_open(path))
            SELECT opened
            CASE Ok(value)
                connection_id = value
                SEND reply_id, Database_Opened(connection_id, "", 0)
            CASE Error(problem)
                SEND reply_id, Database_Opened(0, problem.code, problem.native_code)
            END SELECT
        END IF
    END RECEIVE

    RECEIVE Database_Execute(sql, reply_id)
        IF connection_id = 0 THEN
            SEND reply_id, Database_Execute_Completed(FALSE, 0, 0, "closed", 0)
        ELSE
            executed = parse_execute_result(sqlite_native_execute(connection_id, sql))
            SELECT executed
            CASE Ok(value)
                SEND reply_id, Database_Execute_Completed(TRUE, value[0], value[1], "", 0)
            CASE Error(problem)
                SEND reply_id, Database_Execute_Completed(FALSE, 0, 0, problem.code, problem.native_code)
            END SELECT
        END IF
    END RECEIVE

    RECEIVE Database_Query(sql, reply_id)
        IF connection_id = 0 THEN
            SEND reply_id, Database_Query_Completed(FALSE, "", "closed", 0)
        ELSE
            queried = parse_query_result(sqlite_native_query(connection_id, sql))
            SELECT queried
            CASE Ok(json_text)
                SEND reply_id, Database_Query_Completed(TRUE, json_text, "", 0)
            CASE Error(problem)
                SEND reply_id, Database_Query_Completed(FALSE, "", problem.code, problem.native_code)
            END SELECT
        END IF
    END RECEIVE

    RECEIVE Database_Close(reply_id)
        IF connection_id = 0 THEN
            SEND reply_id, Database_Closed(TRUE, "", 0)
        ELSE
            closed = parse_close_result(sqlite_native_close(connection_id))
            SELECT closed
            CASE Ok(value)
                connection_id = 0
                SEND reply_id, Database_Closed(TRUE, "", 0)
            CASE Error(problem)
                SEND reply_id, Database_Closed(FALSE, problem.code, problem.native_code)
            END SELECT
        END IF
    END RECEIVE
END ACTOR
