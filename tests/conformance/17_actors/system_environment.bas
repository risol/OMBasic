ACTOR Environment_Client
    RECEIVE Environment_Arguments_Completed(outcome)
        arguments = TRY outcome
        IF arguments[0] = "" THEN
            print("arguments=invalid")
            STOP
        END IF
        print("arguments=ok")
        SEND Environment, Environment_Current_Directory(SELF)
    END RECEIVE

    RECEIVE Environment_Current_Directory_Completed(outcome)
        directory = TRY outcome
        IF directory = "" THEN
            print("directory=invalid")
            STOP
        END IF
        print("directory=ok")
        SEND Environment, Environment_Executable_Path(SELF)
    END RECEIVE

    RECEIVE Environment_Executable_Path_Completed(outcome)
        path = TRY outcome
        IF path = "" THEN
            print("executable=invalid")
            STOP
        END IF
        print("executable=ok")
        SEND Environment, Environment_Get("PATH", SELF)
    END RECEIVE

    RECEIVE Environment_Get_Completed(outcome)
        SELECT outcome
        CASE Ok(value)
            IF value = "" THEN
                print("path-variable=empty")
            ELSE
                print("path-variable=ok")
            END IF
            SEND Environment, Environment_Get("", SELF)
        CASE Error(problem)
            IF problem = "invalid_name" THEN
                print("invalid-name=ok")
            ELSE
                print(problem)
            END IF
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN Environment_Client
    SEND Environment, Environment_Arguments(client_ref)
END ACTION
