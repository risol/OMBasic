MESSAGE Start_Process_Limit_Test()

ACTOR Process_Limit_Test
    VAR next_request = 0
    VAR spawn_responses = 0
    VAR spawn_successes = 0
    VAR spawn_failures = 0
    VAR wait_responses = 0

    RECEIVE Start_Process_Limit_Test()
        WHILE next_request < 65
            SEND Process, Process_Spawn("/usr/bin/true", [], TRUE, [], "", SELF)
            next_request = next_request + 1
        END WHILE
    END RECEIVE

    RECEIVE Process_Spawn_Completed(outcome)
        spawn_responses = spawn_responses + 1
        SELECT outcome
        CASE Ok(process_id)
            spawn_successes = spawn_successes + 1
            SEND Process, Process_Wait(process_id, SELF)
        CASE Error(problem)
            IF problem = "resource_exhausted" THEN
                spawn_failures = spawn_failures + 1
            ELSE
                print($"spawn-error={problem}")
                STOP
            END IF
        END SELECT
        IF spawn_responses = 65 AND wait_responses = 64 THEN
            print($"spawned={spawn_successes},rejected={spawn_failures},waited={wait_responses}")
            STOP
        END IF
    END RECEIVE

    RECEIVE Process_Wait_Completed(outcome)
        SELECT outcome
        CASE Ok(exit_code)
            IF exit_code = 0 THEN
                wait_responses = wait_responses + 1
            END IF
        CASE Error(problem)
            print($"wait-error={problem}")
            STOP
        END SELECT
        IF spawn_responses = 65 AND wait_responses = 64 THEN
            print($"spawned={spawn_successes},rejected={spawn_failures},waited={wait_responses}")
            STOP
        END IF
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Process_Limit_Test
    SEND client, Start_Process_Limit_Test()
END ACTION
