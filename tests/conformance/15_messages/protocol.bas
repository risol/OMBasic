MESSAGE Ping(value)
MESSAGE Pong(value)

ACTOR Worker
    RECEIVE Ping(value)
        print($"ping={value}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    worker_ref = SPAWN Worker
    SEND worker_ref, Ping(7)
END ACTION
