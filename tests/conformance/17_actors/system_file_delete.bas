ACTOR Delete_Client
    RECEIVE File_Write_Completed(outcome)
        SELECT outcome
        CASE Ok(done)
            SEND File_System, File_Delete("delete.txt", SELF)
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE File_Delete_Completed(outcome)
        SELECT outcome
        CASE Ok(done)
            SEND File_System, File_Exists("delete.txt", SELF)
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE File_Exists_Completed(outcome)
        SELECT outcome
        CASE Ok(found)
            IF found THEN
                print("delete-failed")
            ELSE
                print("delete-succeeded")
            END IF
            STOP
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN Delete_Client
    SEND File_System, File_Write("delete.txt", "content", client_ref)
END ACTION
