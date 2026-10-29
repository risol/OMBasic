MESSAGE Add(value)

ACTOR Counter
    VAR count = 0
    RECEIVE Add(value)
        count = count + value
        print($"count={count}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    counter_ref = SPAWN Counter
    SEND counter_ref, Add(1)
END ACTION
