MESSAGE Ping(value)

ACTOR Counter
    VAR count = 0

    RECEIVE Ping(value)
        count = count + value
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
