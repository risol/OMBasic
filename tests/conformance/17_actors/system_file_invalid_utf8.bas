ACTOR File_Client
    RECEIVE File_Read_Completed(outcome)
        SELECT outcome
        CASE Ok(contents)
            print("unexpected-read-success")
        CASE Error(problem)
            print("invalid-utf8-error")
        END SELECT
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN File_Client
    SEND File_System, File_Read("invalid-utf8.txt", client_ref)
END ACTION
