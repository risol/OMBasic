MESSAGE Ping(value)

ACTOR Counter
    VAR count = 0
    VAR values = [1, 2]

    RECEIVE Ping(value)
        count = count + value
        values = list_set(values, 0, value)
        STOP
    END RECEIVE
END ACTOR

ACTION spawn_remote(node)
    remote_ref = SPAWN Counter AT node
END ACTION

ACTION main()
    counter_ref = SPAWN Counter
    SEND counter_ref, Ping(1)
END ACTION
