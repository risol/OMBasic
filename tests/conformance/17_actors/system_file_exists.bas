ACTOR Exists_Client
    RECEIVE File_Write_Completed(outcome)
        SELECT outcome
        CASE Ok(done)
            SEND File_System, File_Exists("exists.txt", SELF)
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE File_Exists_Completed(outcome)
        SELECT outcome
        CASE Ok(found)
            IF found THEN
                print("exists=true")
                SEND File_System, File_Exists("missing-exists.txt", SELF)
            ELSE
                print("exists=false")
                STOP
            END IF
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN Exists_Client
    SEND File_System, File_Write("exists.txt", "content", client_ref)
END ACTION
