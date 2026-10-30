MESSAGE Begin_Tls_Contract()

ACTOR Tls_Contract
    RECEIVE Begin_Tls_Contract()
        SEND Network, Tls_Client_Start(1, "localhost", SELF)
    END RECEIVE

    RECEIVE Tls_Client_Start_Completed(outcome)
        SELECT outcome
        CASE Ok(tls_id)
            SEND Network, Tls_Send(tls_id, [1, 2, 3], SELF)
        CASE Error(problem)
            print($"tls-start-error={problem}")
        END SELECT
    END RECEIVE

    RECEIVE Tls_Send_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes_sent)
            SEND Network, Tls_Receive(1, 16, SELF)
        CASE Error(problem)
            print($"tls-send-error={problem}")
        END SELECT
    END RECEIVE

    RECEIVE Tls_Receive_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes)
            SEND Network, Tls_Close(1, SELF)
        CASE Error(problem)
            print($"tls-receive-error={problem}")
        END SELECT
    END RECEIVE

    RECEIVE Tls_Close_Completed(outcome)
        print("tls-close-completed")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Tls_Contract
    SEND client, Begin_Tls_Contract()
END ACTION
