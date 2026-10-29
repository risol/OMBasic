ACTOR Writer
    RECEIVE Console_Write_Completed(outcome)
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    writer_ref = SPAWN Writer
    SEND Console, Console_Write("system-io", writer_ref)
END ACTION
