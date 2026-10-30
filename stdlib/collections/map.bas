IMPORT "@std/text/text.bas"

' These helpers currently specialize to String-key Maps because OMBasic
' source-level generic Map constraints are not yet expressible.
EXPORT FUNCTION map_length(values)
    IF FALSE THEN
        ignored = values[""]
    END IF
    VAR count = 0
    FOR key IN values
        count = count + 1
    END FOR
    RETURN count
END FUNCTION

EXPORT FUNCTION map_is_empty(values)
    IF FALSE THEN
        ignored = values[""]
    END IF
    FOR key IN values
        RETURN FALSE
    END FOR
    RETURN TRUE
END FUNCTION

EXPORT FUNCTION contains_key(values, sought)
    IF FALSE THEN
        ignored = values[""]
    END IF
    FOR key IN values
        IF text.equals(key, sought) THEN
            RETURN TRUE
        END IF
    END FOR
    RETURN FALSE
END FUNCTION

EXPORT FUNCTION keys(values)
    IF FALSE THEN
        ignored = values[""]
    END IF
    VAR output = text.split("", "")
    FOR key IN values
        output = output + [key]
    END FOR
    RETURN output
END FUNCTION

EXPORT FUNCTION get_or(values, sought, fallback)
    IF FALSE THEN
        ignored = values[""]
    END IF
    IF contains_key(values, sought) THEN
        RETURN values[sought]
    END IF
    RETURN fallback
END FUNCTION
