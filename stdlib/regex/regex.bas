IMPORT "@std/number/number.bas"
IMPORT "@std/collections/list.bas"
IMPORT "@std/text/text.bas"
NATIVE IMPORT "./regex_native.omi"

EXPORT RECORD Regex_Match(matched, start, end_index)
EXPORT RECORD Regex_Error(code, byte_offset)

FUNCTION parse_result(value)
    VAR parts = text.split(value, "|")
    IF parts[0] = "ok" AND list.length(parts) = 4 THEN
        matched_result = number.parse_int(parts[1])
        start_result = number.parse_int(parts[2])
        end_result = number.parse_int(parts[3])
        SELECT matched_result
        CASE Ok(matched_value)
            SELECT start_result
            CASE Ok(start_value)
                SELECT end_result
                CASE Ok(end_value)
                    RETURN Ok(Regex_Match(matched_value = 1, start_value, end_value))
                CASE Error(problem)
                    RETURN Error(Regex_Error("invalid_native_result", 0))
                END SELECT
            CASE Error(problem)
                RETURN Error(Regex_Error("invalid_native_result", 0))
            END SELECT
        CASE Error(problem)
            RETURN Error(Regex_Error("invalid_native_result", 0))
        END SELECT
    END IF
    IF list.length(parts) = 3 AND parts[0] = "error" THEN
        offset_result = number.parse_int(parts[2])
        SELECT offset_result
        CASE Ok(offset)
            RETURN Error(Regex_Error(parts[1], offset))
        CASE Error(problem)
            RETURN Error(Regex_Error("invalid_native_result", 0))
        END SELECT
    END IF
    RETURN Error(Regex_Error("invalid_native_result", 0))
END FUNCTION

EXPORT FUNCTION find(pattern, input)
    RETURN parse_result(regex_find_result(pattern, input))
END FUNCTION

