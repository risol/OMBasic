MESSAGE Start_Windows_Process_Shutdown_Test()

ACTOR Windows_Process_Shutdown_Test
    RECEIVE Start_Windows_Process_Shutdown_Test()
        SEND Process, Process_Spawn("C:\\Windows\\System32\\ping.exe",
            ["-t", "127.0.0.1"], FALSE, [], "", SELF)
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
    client = SPAWN Windows_Process_Shutdown_Test
    SEND client, Start_Windows_Process_Shutdown_Test()
END ACTION
