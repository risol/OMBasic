IMPORT "@std/network/address.bas"

' DNS completion payloads carry numeric IP literals in resolver preference
' order. Convert that boundary list to the public typed socket-address model.
EXPORT FUNCTION socket_address_from_ip_literal(ip_literal, port)
    RETURN address.make_socket_address(ip_literal, port)
END FUNCTION
