ACTOR Clock_Client
    RECEIVE Clock_Monotonic_Now_Completed(outcome)
        SELECT outcome
        CASE Ok(milliseconds)
            IF milliseconds >= 0 THEN
                print("monotonic=ok")
                SEND Clock, Clock_Utc_Now(SELF)
            ELSE
                print("invalid_monotonic")
                STOP
            END IF
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Clock_Utc_Now_Completed(outcome)
        SELECT outcome
        CASE Ok(milliseconds)
            IF milliseconds > 0 THEN
                print("utc=ok")
            ELSE
                print("invalid_utc")
            END IF
            STOP
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN Clock_Client
    SEND Clock, Clock_Monotonic_Now(client_ref)
END ACTION
