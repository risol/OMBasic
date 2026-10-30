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

' Return at most the first count elements. Negative counts behave like zero.
EXPORT FUNCTION take(values, count)
    IF count <= 0 THEN
        RETURN []
    END IF
    VAR output = []
    VAR index = 0
    FOR item IN values
        IF index >= count THEN
            RETURN output
        END IF
        output = output + [item]
        index = index + 1
    END FOR
    RETURN output
END FUNCTION

' Return the elements after the first count positions. Negative counts behave like zero.
EXPORT FUNCTION drop(values, count)
    VAR output = []
    VAR index = 0
    FOR item IN values
        IF index >= count THEN
            output = output + [item]
        END IF
        index = index + 1
    END FOR
    RETURN output
END FUNCTION

' Return count elements beginning at start_index, clipped to the List bounds.
EXPORT FUNCTION slice(values, start_index, count)
    VAR output = []
    VAR index = 0
    VAR value_count = length(values)
    IF start_index < 0 OR count <= 0 OR start_index >= value_count THEN
        RETURN output
    END IF
    VAR end_index = value_count
    IF count < value_count - start_index THEN
        end_index = start_index + count
    END IF
    FOR item IN values
        IF index >= start_index AND index < end_index THEN
            output = output + [item]
        END IF
        index = index + 1
    END FOR
    RETURN output
END FUNCTION

' Apply transform to each element, preserving List order.
EXPORT FUNCTION map(values, transform)
    VAR output = []
    FOR item IN values
        output = output + [transform(item)]
    END FOR
    RETURN output
END FUNCTION

' Keep elements for which predicate returns TRUE, preserving List order.
EXPORT FUNCTION filter(values, predicate)
    VAR output = []
    FOR item IN values
        IF predicate(item) THEN
            output = output + [item]
        END IF
    END FOR
    RETURN output
END FUNCTION
