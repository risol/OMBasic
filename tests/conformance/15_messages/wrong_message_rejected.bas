MESSAGE Ping(value)
MESSAGE Pong(value)

ACTOR Worker
    RECEIVE Ping(value)
    END RECEIVE
END ACTOR

ACTION main()
    worker_ref = SPAWN Worker
    SEND worker_ref, Pong(1)
END ACTION
