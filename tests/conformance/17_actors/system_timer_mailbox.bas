MESSAGE Immediate()
MESSAGE Tick()

ACTOR Receiver
    RECEIVE Immediate()
        print("mailbox-first")
    END RECEIVE
    RECEIVE Tick()
        print("timer-second")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    receiver_ref = SPAWN Receiver
    SEND Timer, Timer_After(50, Tick(), receiver_ref)
    SEND receiver_ref, Immediate()
END ACTION
