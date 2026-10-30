IMPORT "@std/network/address.bas"
IMPORT "@std/collections/list.bas"

ACTION main()
    ipv4 = address.parse_ip("192.0.2.1")
    SELECT ipv4
    CASE Ok(value)
        ipv4_text = address.format_ip(value)
        SELECT ipv4_text
        CASE Ok(rendered)
            print($"ipv4={rendered}|bytes={list.length(value.ip_bytes)}")
        CASE Error(problem)
            print($"ipv4-format-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"ipv4-error={problem.code}")
    END SELECT

    ipv6 = address.parse_ip("2001:0db8:0:0:0:0:0:1")
    SELECT ipv6
    CASE Ok(value)
        formatted = address.format_ip(value)
        SELECT formatted
        CASE Ok(rendered)
            print($"ipv6={rendered}")
        CASE Error(problem)
            print($"ipv6-format-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"ipv6-error={problem.code}")
    END SELECT

    socket = address.make_socket_address("2001:db8::1", 443)
    SELECT socket
    CASE Ok(value)
        formatted = address.format_socket_address(value)
        SELECT formatted
        CASE Ok(rendered)
            print($"socket={rendered}")
        CASE Error(problem)
            print($"socket-format-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"socket-error={problem.code}")
    END SELECT

    invalid = address.parse_ip("192.168.001.1")
    SELECT invalid
    CASE Error(problem)
        print($"ambiguous-ipv4={problem.code}")
    CASE Ok(value)
        print("ambiguous-ipv4=accepted")
    END SELECT
END ACTION
