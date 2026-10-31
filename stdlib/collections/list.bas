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
    RETURN list_reverse_copy(values)
END FUNCTION

' Return at most the first count elements. Negative counts behave like zero.
EXPORT FUNCTION take(values, count)
    IF count <= 0 THEN
        RETURN []
    END IF
    RETURN QUERY item IN values
        TAKE count
        SELECT item
    END QUERY
END FUNCTION

' Return the elements after the first count positions. Negative counts behave like zero.
EXPORT FUNCTION drop(values, count)
    IF count <= 0 THEN
        RETURN values
    END IF
    RETURN list_slice_copy(values, count, length(values) - count)
END FUNCTION

' Return count elements beginning at start_index, clipped to the List bounds.
EXPORT FUNCTION slice(values, start_index, count)
    RETURN list_slice_copy(values, start_index, count)
END FUNCTION

' Apply transform to each element, preserving List order.
EXPORT FUNCTION map(values, transform)
    RETURN QUERY item IN values
        SELECT transform(item)
    END QUERY
END FUNCTION

' Keep elements for which predicate returns TRUE, preserving List order.
EXPORT FUNCTION filter(values, predicate)
    RETURN QUERY item IN values
        WHERE predicate(item)
        SELECT item
    END QUERY
END FUNCTION
