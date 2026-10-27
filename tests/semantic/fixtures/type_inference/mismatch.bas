EXPORT RECORD Unused(value)

FUNCTION partial(flag)
    IF flag THEN
        RETURN 1
    END IF
END FUNCTION

ACTION main()
    bad_sum = 1 + "text"
    VAR counter = 1
    counter = "text"
END ACTION
