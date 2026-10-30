MESSAGE Begin_Erased_Send()
MESSAGE Erased_Pong(value)

ACTOR Erased_Receiver
    RECEIVE Erased_Pong(value)
        print($"erased-message={value}")
        STOP
    END RECEIVE
END ACTOR

ACTOR Erased_Sender
    VAR receiver_id = Actor_Id()
    RECEIVE Begin_Erased_Send()
        receiver = SPAWN Erased_Receiver
        receiver_id = Actor_Id(receiver)
        SEND receiver_id, Erased_Pong(42)
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    sender = SPAWN Erased_Sender
    SEND sender, Begin_Erased_Send()
END ACTION
