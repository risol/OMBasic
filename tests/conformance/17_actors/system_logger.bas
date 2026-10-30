MESSAGE Begin_Logger()

ACTOR Logger_Client
    VAR phase = 0

    RECEIVE Begin_Logger()
        phase = 1
        SEND Logger, Logger_Write("first\n", SELF)
    END RECEIVE

    RECEIVE Logger_Write_Completed(outcome)
        SELECT outcome
        CASE Ok(unit_value)
            IF phase = 1 THEN
                phase = 2
                SEND Logger, Logger_Write("second\n", SELF)
            ELSE
                SEND Logger, Logger_Flush(SELF)
            END IF
        CASE Error(problem)
            print($"logger-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Logger_Flush_Completed(outcome)
        SELECT outcome
        CASE Ok(unit_value)
            print("flushed")
        CASE Error(problem)
            print($"flush-error={problem}")
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Logger_Client
    SEND client, Begin_Logger()
END ACTION
