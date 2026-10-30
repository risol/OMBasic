IMPORT "@std/collections/list.bas"

MESSAGE Begin_Secure_Random()

ACTOR Secure_Random_Client
    VAR phase = 0

    RECEIVE Begin_Secure_Random()
        phase = 1
        SEND Secure_Random, Secure_Random_Bytes(32, SELF)
    END RECEIVE

    RECEIVE Secure_Random_Bytes_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes)
            IF phase = 1 THEN
                print($"length={list.length(bytes)}")
                print($"range={all_bytes_in_range(bytes)}")
                phase = 2
                SEND Secure_Random, Secure_Random_Bytes(0, SELF)
            ELSE IF phase = 2 THEN
                print($"empty={list.length(bytes)}")
                phase = 3
                SEND Secure_Random, Secure_Random_Bytes(-1, SELF)
            ELSE
                print("unexpected-success")
                STOP
            END IF
        CASE Error(problem)
            IF phase = 3 THEN
                print($"negative={problem}")
                phase = 4
                SEND Secure_Random, Secure_Random_Bytes(67108865, SELF)
            ELSE IF phase = 4 THEN
                print($"oversized={problem}")
                STOP
            ELSE
                print($"unexpected-error={problem}")
                STOP
            END IF
        END SELECT
    END RECEIVE
END ACTOR

FUNCTION all_bytes_in_range(values)
    FOR value IN values
        IF value < 0 OR value > 255 THEN
            RETURN FALSE
        END IF
    END FOR
    RETURN TRUE
END FUNCTION

ACTION main()
    client = SPAWN Secure_Random_Client
    SEND client, Begin_Secure_Random()
END ACTION
