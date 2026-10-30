IMPORT "@std/collections/list.bas"

MESSAGE Start_Dns_Numeric_Test()

ACTOR Dns_Numeric_Client
    RECEIVE Start_Dns_Numeric_Test()
        SEND Network, Dns_Resolve("127.0.0.1", 443, SELF)
    END RECEIVE

    RECEIVE Dns_Resolve_Completed(outcome)
        SELECT outcome
        CASE Ok(ip_literals)
            IF list.length(ip_literals) > 0 THEN
                print("numeric=ok")
            ELSE
                print("numeric=empty")
            END IF
            STOP
        CASE Error(problem)
            print($"numeric-error={problem}")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Dns_Numeric_Client
    SEND client, Start_Dns_Numeric_Test()
END ACTION
