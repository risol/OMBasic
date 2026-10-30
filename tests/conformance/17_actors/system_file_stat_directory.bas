ACTOR Stat_Client
    RECEIVE Directory_Create_Completed(outcome)
        SELECT outcome
        CASE Ok(done)
            SEND File_System, File_Stat("stat-directory", SELF)
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE File_Stat_Completed(outcome)
        SELECT outcome
        CASE Ok(info)
            IF info[0] = "directory" AND info[2] <> "" THEN
                print($"stat-directory={info[0]}")
            ELSE
                print("invalid_stat")
            END IF
            STOP
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN Stat_Client
    SEND File_System, Directory_Create("stat-directory", client_ref)
END ACTION
