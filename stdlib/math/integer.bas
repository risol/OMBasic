EXPORT FUNCTION add(left, right)
    RETURN (left + right) + 0
END FUNCTION

EXPORT FUNCTION abs(value)
    IF value < 0 THEN
        RETURN 0 - value
    ELSE
        RETURN value
    END IF
END FUNCTION

EXPORT FUNCTION min(left, right)
    IF (left + 0) < right THEN
        RETURN left
    ELSE
        RETURN right
    END IF
END FUNCTION

EXPORT FUNCTION max(left, right)
    IF (left + 0) > right THEN
        RETURN left
    ELSE
        RETURN right
    END IF
END FUNCTION

EXPORT FUNCTION clamp(value, lower, upper)
    IF (lower + 0) > upper THEN
        RETURN value
    ELSE
        IF value < lower THEN
            RETURN lower
        ELSE
            IF value > upper THEN
                RETURN upper
            ELSE
                RETURN value
            END IF
        END IF
    END IF
END FUNCTION

EXPORT FUNCTION sign(value)
    IF value < 0 THEN
        RETURN -1
    END IF
    IF value > 0 THEN
        RETURN 1
    END IF
    RETURN 0
END FUNCTION

EXPORT FUNCTION is_even(value)
    RETURN value % 2 = 0
END FUNCTION

EXPORT FUNCTION is_odd(value)
    RETURN value % 2 <> 0
END FUNCTION
