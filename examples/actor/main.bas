MESSAGE Increment(value)

ACTOR Counter
    VAR count = 0

    RECEIVE Increment(value)
        count = count + value
        print($"count={count}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    counter_ref = SPAWN Counter
    SEND counter_ref, Increment(1)
END ACTION
