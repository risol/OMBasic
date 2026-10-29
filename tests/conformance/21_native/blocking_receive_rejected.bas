MESSAGE Request()

ACTOR Worker
    RECEIVE Request()
        value = wait_for_data()
    END RECEIVE
END ACTOR

ACTION main()
    worker_ref = SPAWN Worker
    SEND worker_ref, Request()
END ACTION
