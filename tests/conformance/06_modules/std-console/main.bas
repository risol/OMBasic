IMPORT "@std/console/console_api.bas"

ACTOR Writer
    RECEIVE Console_Write_Completed(outcome)
        SELECT outcome
        CASE Ok(done)
            STOP
        CASE Error(problem)
            print("console-error")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    first_writer = SPAWN Writer
    second_writer = SPAWN Writer
    SEND Console, Console_Write(console_api.write_payload("console actor"), first_writer)
    SEND Console, Console_Write(console_api.write_line_payload("console actor"), second_writer)
END ACTION
