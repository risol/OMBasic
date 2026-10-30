IMPORT "@std/text/text.bas"
IMPORT "@std/collections/list.bas"

EXPORT RECORD Buffer(bytes)
EXPORT RECORD Encoding_Error(code, offset)

FUNCTION quotient(value, divisor)
    VAR remainder = value
    VAR quotient_value = 0
    WHILE remainder >= divisor
        remainder = remainder - divisor
        quotient_value = quotient_value + 1
    END WHILE
    RETURN quotient_value
END FUNCTION

FUNCTION encoded_byte_offset(value, scalar_index)
    RETURN text.byte_length(text.slice(value, 0, scalar_index))
END FUNCTION

EXPORT FUNCTION buffer_from_bytes(values)
    IF list.length(values) > 67108864 THEN
        RETURN Error(Encoding_Error("resource_exhausted", 67108864))
    END IF
    VAR normalized = []
    VAR index = 0
    FOR value IN values
        IF value < 0 OR value > 255 THEN
            RETURN Error(Encoding_Error("invalid_byte", index))
        END IF
        normalized = normalized + [value + 0]
        index = index + 1
    END FOR
    RETURN Ok(Buffer(normalized))
END FUNCTION

EXPORT FUNCTION buffer_length(value)
    IF NOT valid_buffer(value) THEN
        RETURN Error(Encoding_Error("invalid_buffer", 0))
    END IF
    RETURN Ok(list.length(value.bytes))
END FUNCTION

FUNCTION valid_buffer(value)
    IF list.length(value.bytes) > 67108864 THEN
        RETURN FALSE
    END IF
    FOR byte_value IN value.bytes
        IF byte_value < 0 OR byte_value > 255 THEN
            RETURN FALSE
        END IF
    END FOR
    RETURN TRUE
END FUNCTION

EXPORT FUNCTION hex_encode(value)
    IF NOT valid_buffer(value) THEN
        RETURN Error(Encoding_Error("invalid_buffer", 0))
    END IF
    alphabet = "0123456789abcdef"
    VAR output = ""
    FOR byte_value IN value.bytes
        output = output + text.slice(alphabet, quotient(byte_value, 16), 1)
        output = output + text.slice(alphabet, byte_value % 16, 1)
    END FOR
    RETURN Ok(output)
END FUNCTION

FUNCTION hex_digit(character)
    lower_alphabet = "0123456789abcdef"
    upper_alphabet = "0123456789ABCDEF"
    digit = text.find(lower_alphabet, character)
    IF digit >= 0 THEN
        RETURN digit
    END IF
    RETURN text.find(upper_alphabet, character)
END FUNCTION

EXPORT FUNCTION hex_decode(encoded)
    encoded_length = text.length(encoded)
    encoded_byte_length = text.byte_length(encoded)
    IF encoded_byte_length > 134217728 THEN
        RETURN Error(Encoding_Error("resource_exhausted", 134217728))
    END IF
    IF encoded_length % 2 <> 0 THEN
        RETURN Error(Encoding_Error("invalid_length", encoded_byte_length))
    END IF
    VAR decoded = []
    VAR index = 0
    VAR first_byte = 0
    VAR second_byte = 0
    VAR third_byte = 0
    VAR byte_count = 0
    WHILE index < encoded_length
        high_digit = hex_digit(text.slice(encoded, index, 1))
        low_digit = hex_digit(text.slice(encoded, index + 1, 1))
        IF high_digit < 0 THEN
            RETURN Error(Encoding_Error("invalid_character",
                encoded_byte_offset(encoded, index)))
        END IF
        IF low_digit < 0 THEN
            RETURN Error(Encoding_Error("invalid_character",
                encoded_byte_offset(encoded, index + 1)))
        END IF
        decoded = decoded + [high_digit * 16 + low_digit]
        index = index + 2
    END WHILE
    VAR normalized = []
    FOR byte_value IN decoded
        normalized = normalized + [byte_value + 0]
    END FOR
    RETURN Ok(Buffer(normalized))
END FUNCTION

FUNCTION base64_digit(character)
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    RETURN text.find(alphabet, character)
END FUNCTION

FUNCTION base64_symbol(index)
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    RETURN text.slice(alphabet, index, 1)
END FUNCTION

EXPORT FUNCTION base64_encode(value)
    IF NOT valid_buffer(value) THEN
        RETURN Error(Encoding_Error("invalid_buffer", 0))
    END IF
    VAR output = ""
    VAR position = 0
    VAR first = 0
    VAR second = 0
    VAR third = 0
    FOR byte_value IN value.bytes
        IF position = 0 THEN
            first = byte_value
            position = 1
        ELSE IF position = 1 THEN
            second = byte_value
            position = 2
        ELSE
            third = byte_value
            output = output + base64_symbol(quotient(first, 4))
            output = output + base64_symbol((first % 4) * 16 + quotient(second, 16))
            output = output + base64_symbol((second % 16) * 4 + quotient(third, 64))
            output = output + base64_symbol(third % 64)
            position = 0
        END IF
    END FOR
    IF position = 1 THEN
        output = output + base64_symbol(quotient(first, 4))
        output = output + base64_symbol((first % 4) * 16)
        output = output + "=="
    ELSE IF position = 2 THEN
        output = output + base64_symbol(quotient(first, 4))
        output = output + base64_symbol((first % 4) * 16 + quotient(second, 16))
        output = output + base64_symbol((second % 16) * 4)
        output = output + "="
    END IF
    RETURN Ok(output)
END FUNCTION

EXPORT FUNCTION base64_decode(encoded)
    encoded_length = text.length(encoded)
    encoded_byte_length = text.byte_length(encoded)
    IF encoded_byte_length > 89478488 THEN
        RETURN Error(Encoding_Error("resource_exhausted", 89478488))
    END IF
    IF encoded_length % 4 <> 0 THEN
        RETURN Error(Encoding_Error("invalid_length", encoded_byte_length))
    END IF
    VAR decoded = []
    VAR index = 0
    VAR first_byte = 0
    VAR second_byte = 0
    VAR third_byte = 0
    VAR byte_count = 0
    WHILE index < encoded_length
        first_char = text.slice(encoded, index, 1)
        second_char = text.slice(encoded, index + 1, 1)
        third_char = text.slice(encoded, index + 2, 1)
        fourth_char = text.slice(encoded, index + 3, 1)
        first_value = base64_digit(first_char)
        second_value = base64_digit(second_char)
        third_value = base64_digit(third_char)
        fourth_value = base64_digit(fourth_char)
        final_block = index + 4 = encoded_length
        IF first_value < 0 THEN
            RETURN Error(Encoding_Error("invalid_character",
                encoded_byte_offset(encoded, index)))
        END IF
        IF second_value < 0 THEN
            RETURN Error(Encoding_Error("invalid_character",
                encoded_byte_offset(encoded, index + 1)))
        END IF
        IF third_char = "=" THEN
            IF NOT final_block OR fourth_char <> "=" THEN
                RETURN Error(Encoding_Error("invalid_padding",
                    encoded_byte_offset(encoded, index + 2)))
            END IF
            IF second_value % 16 <> 0 THEN
                RETURN Error(Encoding_Error("non_canonical",
                    encoded_byte_offset(encoded, index + 1)))
            END IF
            first_byte = first_value * 4 + quotient(second_value, 16)
            byte_count = 1
        ELSE
            IF third_value < 0 THEN
                RETURN Error(Encoding_Error("invalid_character",
                    encoded_byte_offset(encoded, index + 2)))
            END IF
            first_byte = first_value * 4 + quotient(second_value, 16)
            IF fourth_char = "=" THEN
                IF NOT final_block THEN
                    RETURN Error(Encoding_Error("invalid_padding",
                        encoded_byte_offset(encoded, index + 3)))
                END IF
                IF third_value % 4 <> 0 THEN
                    RETURN Error(Encoding_Error("non_canonical",
                        encoded_byte_offset(encoded, index + 2)))
                END IF
                second_byte = (second_value % 16) * 16 + quotient(third_value, 4)
                byte_count = 2
            ELSE
                IF fourth_value < 0 THEN
                    RETURN Error(Encoding_Error("invalid_character",
                        encoded_byte_offset(encoded, index + 3)))
                END IF
                second_byte = (second_value % 16) * 16 + quotient(third_value, 4)
                third_byte = (third_value % 4) * 64 + fourth_value
                byte_count = 3
            END IF
        END IF
        decoded = decoded + [first_byte + 0]
        IF byte_count > 1 THEN
            decoded = decoded + [second_byte + 0]
        END IF
        IF byte_count > 2 THEN
            decoded = decoded + [third_byte + 0]
        END IF
        index = index + 4
    END WHILE
    VAR normalized = []
    FOR byte_value IN decoded
        normalized = normalized + [byte_value + 0]
    END FOR
    RETURN buffer_from_bytes(normalized)
END FUNCTION
