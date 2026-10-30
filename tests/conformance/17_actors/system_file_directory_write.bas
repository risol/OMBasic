MESSAGE Error_Timeout()

ACTOR File_Client
    RECEIVE File_Write_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Error_Timeout()
        print("unexpected-write-success")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN File_Client
    SEND File_System, File_Write(".", "contents", client_ref)
    SEND Timer, Timer_After(100, Error_Timeout(), client_ref)
END ACTION
