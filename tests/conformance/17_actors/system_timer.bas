MESSAGE Tick()

ACTOR Receiver
    RECEIVE Tick()
        print("timer-fired")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    receiver_ref = SPAWN Receiver
    SEND Timer, Timer_After(1, Tick(), receiver_ref)
END ACTION
