IMPORT "@std/collections/list.bas"

MESSAGE Start_Udp_Loopback()
MESSAGE Start_First_Udp()
MESSAGE Start_Second_Udp(first_resource_id)

ACTOR Udp_Loopback
    RECEIVE Start_Udp_Loopback()
        first = SPAWN Udp_First
        SEND first, Start_First_Udp()
    END RECEIVE
END ACTOR

ACTOR Udp_First
    RECEIVE Start_First_Udp()
        SEND Network, Udp_Connect("127.0.0.1", 47632, 47631, SELF)
    END RECEIVE

    RECEIVE Udp_Connect_Completed(outcome)
        SELECT outcome
        CASE Ok(resource_id)
            second = SPAWN Udp_Second
            SEND second, Start_Second_Udp(resource_id)
            STOP
        CASE Error(problem)
            print($"first-connect-error={problem}")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTOR Udp_Second
    VAR first_id = 0
    VAR second_id = 0
    VAR sent = FALSE
    VAR received = FALSE

    RECEIVE Start_Second_Udp(first_resource_id)
        first_id = first_resource_id
        SEND Network, Udp_Connect("127.0.0.1", 47631, 47632, SELF)
    END RECEIVE

    RECEIVE Udp_Connect_Completed(outcome)
        SELECT outcome
        CASE Ok(resource_id)
            second_id = resource_id
            SEND Network, Udp_Receive(second_id, 4, SELF)
            SEND Network, Udp_Send(first_id, [65, 66, 67, 68], SELF)
        CASE Error(problem)
            print($"second-connect-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Udp_Send_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes_sent)
            IF bytes_sent <> 4 THEN
                print($"send-count={bytes_sent}")
                STOP
            END IF
            sent = TRUE
            IF received THEN
                STOP
            END IF
        CASE Error(problem)
            print($"send-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Udp_Receive_Completed(outcome)
        SELECT outcome
        CASE Ok(values)
            IF list.length(values) = 4 THEN
                print($"received={values[0]},{values[1]},{values[2]},{values[3]}")
            ELSE
                print($"receive-count={list.length(values)}")
                STOP
            END IF
            received = TRUE
            IF sent THEN
                STOP
            END IF
        CASE Error(problem)
            print($"receive-error={problem}")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    worker = SPAWN Udp_Loopback
    SEND worker, Start_Udp_Loopback()
END ACTION
