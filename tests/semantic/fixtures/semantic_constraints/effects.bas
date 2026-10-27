MESSAGE Ping()

FUNCTION hidden_effect()
    perform_work()
    RETURN 1
END FUNCTION

ACTION perform_work()
END ACTION

ACTION invalid_actor_operations()
    WATCH SPAWN Worker
    STOP
END ACTION

ACTION main()
    hidden_value = hidden_effect()
    invalid_actor_operations()
END ACTION

ACTOR Worker
    VAR base = 1
    VAR derived = base + 1
    RECEIVE Ping()
    END RECEIVE
END ACTOR
