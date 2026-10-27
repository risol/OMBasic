MESSAGE System_Started()
MESSAGE System_Timer_Done()
MESSAGE System_Fail()

ACTION main()
    client = SPAWN System_Client
    SEND client, System_Started()
END ACTION

ACTION fail_while_timer_is_pending()
    client = SPAWN System_Client
    SEND client, System_Fail()
END ACTION

ACTOR System_Client
    VAR completions = 0
    RECEIVE System_Started()
        SEND Console, Console_Write("system actors active\n", SELF)
        SEND File_System, File_Write("system_actor_test.tmp", "System Actor I/O", SELF)
        SEND Timer, Timer_After(10, System_Timer_Done(), SELF)
    END RECEIVE
    RECEIVE System_Fail()
        SEND Timer, Timer_After(60000, System_Timer_Done(), SELF)
        trigger = 1 / 0
    END RECEIVE
    RECEIVE Console_Write_Completed(outcome)
        ignored = TRY outcome
        completions = completions + 1
        IF completions = 3 THEN
            STOP
        END IF
    END RECEIVE
    RECEIVE File_Write_Completed(outcome)
        ignored = TRY outcome
        SEND File_System, File_Read("system_actor_test.tmp", SELF)
    END RECEIVE
    RECEIVE File_Read_Completed(outcome)
        contents = TRY outcome
        IF contents <> "System Actor I/O" THEN
            trigger = 1 / 0
        END IF
        completions = completions + 1
        IF completions = 3 THEN
            STOP
        END IF
    END RECEIVE
    RECEIVE System_Timer_Done()
        completions = completions + 1
        IF completions = 3 THEN
            STOP
        END IF
    END RECEIVE
END ACTOR
