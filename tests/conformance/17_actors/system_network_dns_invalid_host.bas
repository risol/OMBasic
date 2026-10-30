MESSAGE Start_Dns_Invalid_Host_Test()

ACTOR Dns_Invalid_Host_Client
    RECEIVE Start_Dns_Invalid_Host_Test()
        SEND Network, Dns_Resolve("invalid_host", 80, SELF)
    END RECEIVE

    RECEIVE Dns_Resolve_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            IF problem = "invalid_host" THEN
                print("empty-host=invalid_host")
            ELSE
                print($"empty-host={problem}")
            END IF
            STOP
        CASE Ok(ip_literals)
            print("empty-host=accepted")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Dns_Invalid_Host_Client
    SEND client, Start_Dns_Invalid_Host_Test()
END ACTION
