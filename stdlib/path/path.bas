IMPORT "@std/text/text.bas"

EXPORT FUNCTION is_absolute(value)
    length = text.length(value)
    IF length = 0 THEN
        RETURN FALSE
    END IF
    IF text.slice(value, 0, 1) = "/" THEN
        RETURN TRUE
    END IF
    IF length >= 3 AND text.slice(value, 1, 2) = ":/" THEN
        RETURN TRUE
    END IF
    RETURN FALSE
END FUNCTION

EXPORT FUNCTION join(base, child)
    IF is_absolute(child) THEN
        RETURN child + ""
    END IF
    IF text.is_empty(base) THEN
        RETURN child + ""
    END IF
    IF text.is_empty(child) THEN
        RETURN base + ""
    END IF
    base_length = text.length(base)
    IF text.slice(base, base_length - 1, 1) = "/" THEN
        RETURN (base + child) + ""
    END IF
    RETURN ((base + "/") + child) + ""
END FUNCTION

EXPORT FUNCTION basename(value)
    end_index = text.length(value)
    separator = text.find_last(value, "/")
    RETURN text.slice(value, separator + 1, end_index - separator - 1)
END FUNCTION

EXPORT FUNCTION dirname(value)
    separator = text.find_last(value, "/")
    IF separator < 0 THEN
        RETURN "."
    END IF
    IF separator = 0 THEN
        RETURN "/"
    END IF
    IF separator = 2 AND text.slice(value, 1, 2) = ":/" THEN
        RETURN text.slice(value, 0, 3)
    END IF
    RETURN text.slice(value, 0, separator)
END FUNCTION

EXPORT FUNCTION extension(value)
    length = text.length(value)
    separator = text.find_last(value, "/")
    VAR name_start = separator + 1
    dot = text.find_last(value, ".")
    IF dot <= name_start OR dot = length - 1 OR dot < separator THEN
        RETURN ""
    END IF
    RETURN text.slice(value, dot, length - dot)
END FUNCTION
