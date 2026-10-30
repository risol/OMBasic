MESSAGE Begin_Read(value)

ACTOR Stop_After_Request
    RECEIVE Begin_Read(value)
        SEND Console, Console_Read_Line(SELF)
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    reader = SPAWN Stop_After_Request
    SEND reader, Begin_Read(0)
END ACTION
