IMPORT "@std/collections/list.bas"

MESSAGE Start_Dns_Test()

ACTOR Dns_Test_Client
    RECEIVE Start_Dns_Test()
        SEND Network, Dns_Resolve("localhost", 80, SELF)
    END RECEIVE

    RECEIVE Dns_Resolve_Completed(outcome)
        SELECT outcome
        CASE Ok(ip_literals)
            IF list.length(ip_literals) > 0 THEN
                print("localhost=ok")
            ELSE
                print("localhost=empty")
            END IF
            STOP
        CASE Error(problem)
            print($"localhost-error={problem}")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Dns_Test_Client
    SEND client, Start_Dns_Test()
END ACTION
