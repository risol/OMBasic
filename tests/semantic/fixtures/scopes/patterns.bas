MESSAGE Ping(value)

ACTOR Worker
    VAR count = 0
    RECEIVE Ping(value)
        count = count + value
    END RECEIVE
END ACTOR

ACTION main()
    outcome = Ok(1)
    SELECT outcome
    CASE Ok(value)
        selected_value = value
    CASE Error(problem)
        selected_error = problem
    CASE ELSE
        fallback = 0
    END SELECT
    FOR index = 1 TO 2
        numeric_item = index
    END FOR
    FOR element IN [1, 2]
        collection_item = element
    END FOR
    spawned_actor = SPAWN Worker
END ACTION
