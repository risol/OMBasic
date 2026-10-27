MESSAGE Ping(value)
MESSAGE System_Start()
MESSAGE System_Done(ok)

FUNCTION sum_to(limit)
    VAR total = 0
    FOR i = 1 TO limit
        total = total + i
    END FOR
    RETURN total
END FUNCTION

FUNCTION unwrap(outcome)
    value = TRY outcome
    RETURN Ok(value)
END FUNCTION

FUNCTION choose(outcome)
    SELECT outcome
    CASE Ok(value)
        RETURN value
    CASE Error(problem)
        RETURN 0
    END SELECT
    RETURN 0
END FUNCTION

ACTION main()
    worker_ref = SPAWN Worker
    SEND worker_ref, Ping(sum_to(3))
    system_ref = SPAWN System_Client
    SEND system_ref, System_Start()
    ignored = choose(unwrap(Ok(1)))
END ACTION

ACTOR Worker
    VAR count = 0
    RECEIVE Ping(value)
        count = count + value
    END RECEIVE
END ACTOR

ACTOR System_Client
    RECEIVE System_Start()
        SEND Console, Console_Write("ready", SELF)
        SEND File_System, File_Read("sample.txt", SELF)
        SEND File_System, File_Write("sample.txt", "contents", SELF)
        SEND Timer, Timer_After(0, System_Done(TRUE), SELF)
    END RECEIVE
    RECEIVE Console_Write_Completed(completion)
    END RECEIVE
    RECEIVE File_Read_Completed(completion)
    END RECEIVE
    RECEIVE File_Write_Completed(completion)
    END RECEIVE
    RECEIVE System_Done(finished)
    END RECEIVE
END ACTOR
