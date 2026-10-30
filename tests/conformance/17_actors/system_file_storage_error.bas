MESSAGE Write_Timeout()

ACTOR File_Client
    RECEIVE File_Write_Completed(outcome)
        SELECT outcome
        CASE Error(problem)
            print(problem)
            STOP
        CASE Ok(done)
            print("unexpected-write-success")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Write_Timeout()
        print("write-timeout")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN File_Client
    SEND File_System, File_Write("/dev/full", "contents", client_ref)
    SEND Timer, Timer_After(1000, Write_Timeout(), client_ref)
END ACTION
