FUNCTION parse_value(valid)
    IF valid THEN
        RETURN Ok(42)
    ELSE
        RETURN Error("invalid")
    END IF
END FUNCTION

ACTION main()
    outcome = parse_value(TRUE)
    SELECT outcome
    CASE Ok(value)
        print($"ok={value}")
    CASE Error(problem)
        print($"error={problem}")
    END SELECT
END ACTION
