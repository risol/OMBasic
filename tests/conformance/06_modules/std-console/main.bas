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
    writer_ref = SPAWN Writer
    payload = console_api.write_payload("console actor") + console_api.write_line_payload("console actor")
    SEND Console, Console_Write(payload, writer_ref)
END ACTION
