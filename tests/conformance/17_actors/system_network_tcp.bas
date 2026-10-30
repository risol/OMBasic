IMPORT "@std/collections/list.bas"
IMPORT "@std/encoding.bas"
IMPORT "@std/network/tcp.bas"

MESSAGE Start_Tcp_Loopback()

ACTOR Tcp_Loopback_Client
    RECEIVE Start_Tcp_Loopback()
        SEND Network, Tcp_Listen("127.0.0.1", 47621, 8, SELF)
    END RECEIVE

    RECEIVE Tcp_Listen_Completed(outcome)
        SELECT outcome
        CASE Ok(listener_id)
            SEND Network, Tcp_Accept(listener_id, SELF)
            SEND Network, Tcp_Connect("127.0.0.1", 47621, SELF)
        CASE Error(problem)
            print($"listen-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Accept_Completed(outcome)
        SELECT outcome
        CASE Ok(connection_id)
            SEND Network, Tcp_Receive(connection_id, 4, SELF)
        CASE Error(problem)
            print($"accept-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Connect_Completed(outcome)
        SELECT outcome
        CASE Ok(connection_id)
            SEND Network, Tcp_Send(connection_id,
                tcp.payload_bytes(encoding.Buffer([65, 66, 67, 68])), SELF)
        CASE Error(problem)
            print($"connect-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Send_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes_sent)
            IF bytes_sent <> 4 THEN
                print($"send-count={bytes_sent}")
                STOP
            END IF
        CASE Error(problem)
            print($"send-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tcp_Receive_Completed(outcome)
        SELECT outcome
        CASE Ok(values)
            IF list.length(values) = 4 THEN
                print($"received={values[0]},{values[1]},{values[2]},{values[3]}")
            ELSE
                print($"receive-count={list.length(values)}")
            END IF
            STOP
        CASE Error(problem)
            print($"receive-error={problem}")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Tcp_Loopback_Client
    SEND client, Start_Tcp_Loopback()
END ACTION
