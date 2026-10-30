MESSAGE Start_Process_Shutdown_Test()

ACTOR Process_Shutdown_Test
    RECEIVE Start_Process_Shutdown_Test()
        SEND Process, Process_Spawn("/usr/bin/sleep", ["60"], TRUE,
            [], "", SELF)
    END RECEIVE

    RECEIVE Process_Spawn_Completed(outcome)
        SELECT outcome
        CASE Ok(process_id)
            print("spawned")
        CASE Error(problem)
            print($"spawn-error={problem}")
        END SELECT
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Process_Shutdown_Test
    SEND client, Start_Process_Shutdown_Test()
END ACTION
