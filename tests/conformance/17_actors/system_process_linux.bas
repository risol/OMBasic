MESSAGE Start_Process_Test()

ACTOR Process_Test
    VAR process_id_value = 0
    RECEIVE Start_Process_Test()
        SEND Process, Process_Spawn("/usr/bin/printf",
            ["%s", "A;B C"], TRUE, [], "", SELF)
    END RECEIVE

    RECEIVE Process_Spawn_Completed(outcome)
        SELECT outcome
        CASE Ok(process_id)
            SEND Process, Process_Read_Stdout(process_id, 128, SELF)
            process_id_value = process_id
        CASE Error(problem)
            print($"spawn-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Process_Read_Stdout_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes)
            print($"stdout={bytes[0]},{bytes[1]},{bytes[2]},{bytes[3]},{bytes[4]}")
            SEND Process, Process_Wait(process_id_value, SELF)
        CASE Error(problem)
            print($"read-error={problem}")
        END SELECT
    END RECEIVE

    RECEIVE Process_Wait_Completed(outcome)
        SELECT outcome
        CASE Ok(exit_code)
            print($"exit={exit_code}")
        CASE Error(problem)
            print($"wait-error={problem}")
        END SELECT
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Process_Test
    SEND client, Start_Process_Test()
END ACTION
