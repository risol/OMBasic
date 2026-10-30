IMPORT "@std/collections/list.bas"

MESSAGE Start_Process_Environment_Test()

ACTOR Process_Environment_Test
    VAR process_id_value = 0

    RECEIVE Start_Process_Environment_Test()
        SEND Process, Process_Spawn("/usr/bin/env", [], TRUE,
            ["ONLY=VALUE"], "", SELF)
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

    RECEIVE Process_Wait_Completed(outcome)
        SELECT outcome
        CASE Ok(exit_code)
            IF exit_code = 0 THEN
                SEND Process, Process_Read_Stdout(process_id_value, 128, SELF)
            ELSE
                print($"exit={exit_code}")
                STOP
            END IF
        CASE Error(problem)
            print($"wait-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Process_Read_Stdout_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes)
            IF list.length(bytes) = 11 THEN
                print("environment=replaced")
            ELSE
                print($"environment-length={list.length(bytes)}")
            END IF
        CASE Error(problem)
            print($"read-error={problem}")
        END SELECT
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Process_Environment_Test
    SEND client, Start_Process_Environment_Test()
END ACTION
