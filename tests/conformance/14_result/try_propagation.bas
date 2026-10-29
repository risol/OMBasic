FUNCTION parse_value(valid)
    IF valid THEN
        RETURN Ok(41)
    ELSE
        RETURN Error("invalid")
    END IF
END FUNCTION

FUNCTION add_one(valid)
    value = TRY parse_value(valid)
    RETURN Ok(value + 1)
END FUNCTION

ACTION main()
    SELECT add_one(TRUE)
    CASE Ok(value)
        print($"ok={value}")
    CASE Error(problem)
        print("unexpected success error")
    END SELECT

    SELECT add_one(FALSE)
    CASE Ok(value)
        print("unexpected error success")
    CASE Error(problem)
        print($"error={problem}")
    END SELECT
END ACTION
