EXPORT RECORD Instant(milliseconds)

FUNCTION int_value(value)
    RETURN value + (value % 1)
END FUNCTION

EXPORT FUNCTION instant_from_milliseconds(value)
    RETURN Instant(int_value(value))
END FUNCTION

EXPORT FUNCTION instant_to_milliseconds(value)
    RETURN int_value(value.milliseconds)
END FUNCTION

EXPORT FUNCTION instant_compare(left, right)
    IF int_value(left.milliseconds) < int_value(right.milliseconds) THEN
        RETURN -1
    END IF
    IF int_value(left.milliseconds) > int_value(right.milliseconds) THEN
        RETURN 1
    END IF
    RETURN 0
END FUNCTION

EXPORT FUNCTION instant_difference_milliseconds(start, finish)
    RETURN int_value(finish.milliseconds) - int_value(start.milliseconds)
END FUNCTION
