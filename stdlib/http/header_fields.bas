IMPORT "@std/collections/list.bas"
IMPORT "@std/text/text.bas"

FUNCTION ascii_lower(character)
    upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    lower = "abcdefghijklmnopqrstuvwxyz"
    position = text.find(upper, character)
    IF position >= 0 THEN
        RETURN text.slice(lower, position, 1)
    END IF
    RETURN character
END FUNCTION

EXPORT FUNCTION name_equals(left, right)
    IF text.length(left) <> text.length(right) THEN
        RETURN FALSE
    END IF
    VAR index = 0
    WHILE index < text.length(left)
        IF ascii_lower(text.slice(left, index, 1)) <> ascii_lower(text.slice(right, index, 1)) THEN
            RETURN FALSE
        END IF
        index = index + 1
    END WHILE
    RETURN TRUE
END FUNCTION

EXPORT FUNCTION valid_name(name)
    IF name = "" THEN
        RETURN FALSE
    END IF
    token = "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    FOR character IN text.split(name, "")
        IF text.find(token, character) < 0 THEN
            RETURN FALSE
        END IF
    END FOR
    RETURN TRUE
END FUNCTION

EXPORT FUNCTION valid_value(value)
    FOR character IN text.split(value, "")
        IF character = "\r" OR character = "\n" THEN
            RETURN FALSE
        END IF
        IF text.find("\t", character) < 0 AND text.find(" ", character) < 0 AND text.find("0123456789", character) < 0 AND text.find("ABCDEFGHIJKLMNOPQRSTUVWXYZ", character) < 0 AND text.find("abcdefghijklmnopqrstuvwxyz", character) < 0 AND text.find("!#$%&'*+-.^_`|~", character) < 0 THEN
            RETURN FALSE
        END IF
    END FOR
    RETURN TRUE
END FUNCTION

EXPORT FUNCTION line_valid(line)
    colon = text.find(line, ":")
    IF colon <= 0 THEN
        RETURN FALSE
    END IF
    name = text.slice(line, 0, colon)
    value = text.trim(text.slice(line, colon + 1, text.length(line) - colon - 1))
    RETURN valid_name(name) AND valid_value(value)
END FUNCTION

EXPORT FUNCTION list_valid(values)
    IF list.length(values) > 256 THEN
        RETURN FALSE
    END IF
    VAR total_bytes = 0
    FOR line IN values
        IF NOT line_valid(line) THEN
            RETURN FALSE
        END IF
        total_bytes = total_bytes + text.byte_length(line) + 2
        IF total_bytes > 65536 THEN
            RETURN FALSE
        END IF
    END FOR
    RETURN TRUE
END FUNCTION

EXPORT FUNCTION value_for(values, name)
    FOR line IN values
        colon = text.find(line, ":")
        IF colon > 0 AND name_equals(text.slice(line, 0, colon), name) THEN
            RETURN text.trim(text.slice(line, colon + 1, text.length(line) - colon - 1))
        END IF
    END FOR
    RETURN ""
END FUNCTION

EXPORT FUNCTION count(values, name)
    VAR occurrences = 0
    FOR line IN values
        colon = text.find(line, ":")
        IF colon > 0 AND name_equals(text.slice(line, 0, colon), name) THEN
            occurrences = occurrences + 1
        END IF
    END FOR
    RETURN occurrences
END FUNCTION
