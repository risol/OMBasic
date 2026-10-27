MESSAGE Start()
MESSAGE Tick(value)

ACTOR Callback_Target
    RECEIVE Start()
        native_fire()
    END RECEIVE

    RECEIVE Tick(value)
        IF value > 0 THEN
            STOP
        END IF
    END RECEIVE
END ACTOR

ACTION main()
    target = SPAWN Callback_Target
    SEND target, Start()
END ACTION
