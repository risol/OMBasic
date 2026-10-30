IMPORT "@std/collections/list.bas"

MESSAGE Start_Windows_Process_Test()

ACTOR Windows_Process_Test
    VAR process_id_value = 0

    RECEIVE Start_Windows_Process_Test()
        SEND Process, Process_Spawn("C:\\Windows\\System32\\whoami.exe",
            [], FALSE, [], "", SELF)
    END RECEIVE

    RECEIVE Process_Spawn_Completed(outcome)
        SELECT outcome
        CASE Ok(process_id)
            process_id_value = process_id
            SEND Process, Process_Wait(process_id, SELF)
        CASE Error(problem)
            print($"spawn-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Process_Read_Stdout_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes)
            IF list.length(bytes) > 0 THEN
                print("stdout-received")
            ELSE
                print("stdout-empty")
            END IF
        CASE Error(problem)
            print($"read-error={problem}")
        END SELECT
        STOP
    END RECEIVE

    RECEIVE Process_Wait_Completed(outcome)
        SELECT outcome
        CASE Ok(exit_code)
            print($"exit={exit_code}")
            SEND Process, Process_Read_Stdout(process_id_value, 128, SELF)
        CASE Error(problem)
            print($"wait-error={problem}")
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Windows_Process_Test
    SEND client, Start_Windows_Process_Test()
END ACTION
