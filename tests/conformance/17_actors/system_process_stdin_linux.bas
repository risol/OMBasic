MESSAGE Start_Process_Stdin_Test()

ACTOR Process_Stdin_Test
    VAR process_id_value = 0

    RECEIVE Start_Process_Stdin_Test()
        SEND Process, Process_Spawn("/bin/cat", [], TRUE, [], "", SELF)
    END RECEIVE

    RECEIVE Process_Spawn_Completed(outcome)
        SELECT outcome
        CASE Ok(process_id)
            process_id_value = process_id
            SEND Process, Process_Write_Stdin(process_id, [65, 66], SELF)
        CASE Error(problem)
            print($"spawn-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Process_Write_Stdin_Completed(outcome)
        SELECT outcome
        CASE Ok(byte_count)
            IF byte_count = 2 THEN
                SEND Process, Process_Close_Stdin(process_id_value, SELF)
            ELSE
                print($"write-count={byte_count}")
                STOP
            END IF
        CASE Error(problem)
            print($"write-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Process_Close_Stdin_Completed(outcome)
        SELECT outcome
        CASE Ok(unit_value)
            SEND Process, Process_Read_Stdout(process_id_value, 16, SELF)
        CASE Error(problem)
            print($"close-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Process_Read_Stdout_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes)
            print($"stdout={bytes[0]},{bytes[1]}")
            SEND Process, Process_Wait(process_id_value, SELF)
        CASE Error(problem)
            print($"read-error={problem}")
            STOP
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
    client = SPAWN Process_Stdin_Test
    SEND client, Start_Process_Stdin_Test()
END ACTION
