EXPORT FUNCTION length(values)
    VAR count = 0
    FOR item IN values
        count = count + 1
    END FOR
    RETURN count
END FUNCTION

EXPORT FUNCTION is_empty(values)
    FOR item IN values
        RETURN FALSE
    END FOR
    RETURN TRUE
END FUNCTION

EXPORT FUNCTION contains(values, sought)
    FOR item IN values
        IF item = sought THEN
            RETURN TRUE
        END IF
    END FOR
    RETURN FALSE
END FUNCTION

' Returns the first zero-based index, or -1 when the value is absent.
EXPORT FUNCTION index_of(values, sought)
    VAR index = 0
    FOR item IN values
        IF item = sought THEN
            RETURN index
        END IF
        index = index + 1
    END FOR
    RETURN -1
END FUNCTION

EXPORT FUNCTION concat(left, right)
    RETURN left + right
END FUNCTION

EXPORT FUNCTION reverse(values)
    VAR count = length(values)
    IF count = 0 THEN
        RETURN values
    END IF
    VAR index = count - 1
    VAR output = [values[index]]
    index = index - 1
    WHILE index >= 0
        output = output + [values[index]]
        index = index - 1
    END WHILE
    RETURN output
END FUNCTION
