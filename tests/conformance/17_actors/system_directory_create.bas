ACTOR Directory_Client
    VAR write_count = 0

    RECEIVE Directory_Create_Completed(outcome)
        SELECT outcome
        CASE Ok(done)
            SEND File_System, File_Write("parent/sub/z.txt", "z", SELF)
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE File_Write_Completed(outcome)
        SELECT outcome
        CASE Ok(done)
            IF write_count = 0 THEN
                write_count = 1
                SEND File_System, File_Write("parent/sub/a.txt", "a", SELF)
            ELSE
                SEND File_System, Directory_List("parent/sub", SELF)
            END IF
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Directory_List_Completed(outcome)
        SELECT outcome
        CASE Ok(names)
            print($"directory={names[0]}|{names[1]}")
            SEND File_System, File_Stat("parent/sub/a.txt", SELF)
        CASE Error(problem)
            print(problem)
            STOP
        END SELECT
    END RECEIVE

    RECEIVE File_Stat_Completed(outcome)
        SELECT outcome
        CASE Ok(info)
            IF info[0] = "file" AND info[1] = "1" AND info[2] <> "" THEN
                print($"stat={info[0]}|{info[1]}")
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
    client_ref = SPAWN Directory_Client
    SEND File_System, Directory_Create("parent/sub", client_ref)
END ACTION
