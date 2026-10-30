EXPORT IMPORT "./errors.bas"
EXPORT IMPORT "./header_fields.bas"
EXPORT IMPORT "./request.bas"
EXPORT IMPORT "./response.bas"

EXPORT FUNCTION make_header(name, value)
    RETURN name + ": " + value
END FUNCTION

EXPORT FUNCTION request_target(path, query_present, query)
    VAR target = path
    IF target = "" THEN
        target = "/"
    END IF
    IF query_present THEN
        RETURN target + "?" + query
    END IF
    RETURN target
END FUNCTION

