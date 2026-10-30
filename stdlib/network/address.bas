NATIVE IMPORT "./address_native.omi"
IMPORT "@std/collections/list.bas"
IMPORT "@std/number/number.bas"
IMPORT "@std/text/text.bas"

EXPORT RECORD Ip_Address(ip_family, ip_bytes)
EXPORT RECORD Socket_Address(socket_family, socket_bytes, port)
EXPORT RECORD Address_Error(code)

FUNCTION parsed_ip_bytes(value, family)
    IF family = 4 THEN
        RETURN [address_ip_byte(value, 0), address_ip_byte(value, 1),
            address_ip_byte(value, 2), address_ip_byte(value, 3)]
    END IF
    RETURN [address_ip_byte(value, 0), address_ip_byte(value, 1),
        address_ip_byte(value, 2), address_ip_byte(value, 3),
        address_ip_byte(value, 4), address_ip_byte(value, 5),
        address_ip_byte(value, 6), address_ip_byte(value, 7),
        address_ip_byte(value, 8), address_ip_byte(value, 9),
        address_ip_byte(value, 10), address_ip_byte(value, 11),
        address_ip_byte(value, 12), address_ip_byte(value, 13),
        address_ip_byte(value, 14), address_ip_byte(value, 15)]
END FUNCTION

FUNCTION address_bytes_hex(family, values)
    IF family = 4 THEN
        IF list.length(values) <> 4 THEN
            RETURN ""
        END IF
    ELSE IF family = 6 THEN
        IF list.length(values) <> 16 THEN
            RETURN ""
        END IF
    ELSE
        RETURN ""
    END IF
    alphabet = "0123456789abcdef"
    VAR output = ""
    FOR value IN values
        IF value < 0 OR value > 255 THEN
            RETURN ""
        END IF
        VAR high = 0
        VAR low = value
        WHILE low >= 16
            low = low - 16
            high = high + 1
        END WHILE
        output = output + text.slice(alphabet, high, 1) + text.slice(alphabet, low, 1)
    END FOR
    RETURN output
END FUNCTION

EXPORT FUNCTION parse_ip(value)
    IF address_ip_status(value) <> 0 THEN
        RETURN Error(Address_Error("invalid_address"))
    END IF
    family = address_ip_family(value)
    RETURN Ok(Ip_Address(family, parsed_ip_bytes(value, family)))
END FUNCTION

EXPORT FUNCTION format_ip(value)
    bytes_hex = address_bytes_hex(value.ip_family, value.ip_bytes)
    IF bytes_hex = "" THEN
        RETURN Error(Address_Error("invalid_address"))
    END IF
    formatted = address_ip_format(value.ip_family, bytes_hex)
    IF formatted = "" THEN
        RETURN Error(Address_Error("invalid_address"))
    END IF
    RETURN Ok(formatted)
END FUNCTION

EXPORT FUNCTION make_socket_address(address, port)
    IF port < 0 OR port > 65535 THEN
        RETURN Error(Address_Error("invalid_port"))
    END IF
    IF address_ip_status(address) <> 0 THEN
        RETURN Error(Address_Error("invalid_address"))
    END IF
    family = address_ip_family(address)
    RETURN Ok(Socket_Address(family,
        parsed_ip_bytes(address, family), port))
END FUNCTION

EXPORT FUNCTION format_socket_address(value)
    IF value.port < 0 OR value.port > 65535 THEN
        RETURN Error(Address_Error("invalid_port"))
    END IF
    bytes_hex = address_bytes_hex(value.socket_family, value.socket_bytes)
    IF bytes_hex = "" THEN
        RETURN Error(Address_Error("invalid_address"))
    END IF
    address_text = address_ip_format(value.socket_family, bytes_hex)
    IF address_text = "" THEN
        RETURN Error(Address_Error("invalid_address"))
    END IF
    IF value.socket_family = 6 THEN
        RETURN Ok("[" + address_text + "]:" + number.format_int(value.port))
    END IF
    RETURN Ok(address_text + ":" + number.format_int(value.port))
END FUNCTION
