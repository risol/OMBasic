MESSAGE Forward_To(reply_id)
MESSAGE Forwarded(value)

ACTOR Forwarder
    VAR reply_to = Actor_Id()
    RECEIVE Forward_To(reply_id)
        reply_to = reply_id
        SEND reply_to, Forwarded(42)
    END RECEIVE
END ACTOR

ACTOR Forwarded_Receiver
    RECEIVE Forwarded(value)
        print($"forwarded={value}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    dispatcher = SPAWN Forwarder
    receiver = SPAWN Forwarded_Receiver
    SEND dispatcher, Forward_To(Actor_Id(receiver))
END ACTION
