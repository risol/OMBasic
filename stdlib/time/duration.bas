EXPORT RECORD Duration(milliseconds)

EXPORT FUNCTION from_milliseconds(value)
    RETURN Duration(value)
END FUNCTION

EXPORT FUNCTION to_milliseconds(value)
    RETURN value.milliseconds
END FUNCTION

EXPORT FUNCTION add(left, right)
    RETURN Duration(left.milliseconds + right.milliseconds)
END FUNCTION

EXPORT FUNCTION subtract(left, right)
    RETURN Duration(left.milliseconds - right.milliseconds)
END FUNCTION

EXPORT FUNCTION compare(left, right)
    IF left.milliseconds < right.milliseconds THEN
        RETURN -1
    END IF
    IF left.milliseconds > right.milliseconds THEN
        RETURN 1
    END IF
    RETURN 0
END FUNCTION
