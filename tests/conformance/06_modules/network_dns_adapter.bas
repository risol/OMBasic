IMPORT "@std/network/dns.bas"
IMPORT "@std/network/address.bas"

ACTION main()
    converted = dns.socket_address_from_ip_literal("127.0.0.1", 80)
    SELECT converted
    CASE Ok(value)
        formatted = address.format_socket_address(value)
        SELECT formatted
        CASE Ok(text)
            print($"socket={text}")
        CASE Error(problem)
            print($"format-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"convert-error={problem.code}")
    END SELECT
END ACTION
