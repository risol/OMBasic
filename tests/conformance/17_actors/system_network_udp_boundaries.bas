IMPORT "@std/collections/list.bas"

MESSAGE Start_Udp_Boundaries()
MESSAGE Start_Udp_Boundary_Peer(first_resource_id)

ACTOR Udp_Boundary_Root
    RECEIVE Start_Udp_Boundaries()
        first = SPAWN Udp_Boundary_First
        SEND first, Start_Udp_Boundaries()
    END RECEIVE
END ACTOR

ACTOR Udp_Boundary_First
    RECEIVE Start_Udp_Boundaries()
        SEND Network, Udp_Connect("127.0.0.1", 47642, 47641, SELF)
    END RECEIVE

    RECEIVE Udp_Connect_Completed(outcome)
        SELECT outcome
        CASE Ok(resource_id)
            peer = SPAWN Udp_Boundary_Peer
            SEND peer, Start_Udp_Boundary_Peer(resource_id)
            STOP
        CASE Error(problem)
            print($"first-connect-error={problem}")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTOR Udp_Boundary_Peer
    VAR first_id = 0
    VAR second_id = 0

    RECEIVE Start_Udp_Boundary_Peer(first_resource_id)
        first_id = first_resource_id
        SEND Network, Udp_Connect("127.0.0.1", 47641, 47642, SELF)
    END RECEIVE

    RECEIVE Udp_Connect_Completed(outcome)
        SELECT outcome
        CASE Ok(resource_id)
            second_id = resource_id
            SEND Network, Udp_Send(first_id, [1, 2, 3, 4], SELF)
        CASE Error(problem)
            print($"second-connect-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Udp_Send_Completed(outcome)
        SELECT outcome
        CASE Ok(byte_count)
            IF byte_count = 4 THEN
                SEND Network, Udp_Receive(second_id, 2, SELF)
            ELSE IF byte_count = 0 THEN
                SEND Network, Udp_Receive(second_id, 1, SELF)
            ELSE
                print($"send-count={byte_count}")
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
            IF list.length(values) = 0 THEN
                print("empty-datagram=preserved")
                STOP
            ELSE
                print("oversized-datagram-was-truncated")
                STOP
            END IF
        CASE Error(problem)
            IF problem = "datagram_too_large" THEN
                print("oversized-datagram=rejected")
                SEND Network, Udp_Send(first_id, [], SELF)
            ELSE
                print($"receive-error={problem}")
                STOP
            END IF
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    worker = SPAWN Udp_Boundary_Root
    SEND worker, Start_Udp_Boundaries()
END ACTION
