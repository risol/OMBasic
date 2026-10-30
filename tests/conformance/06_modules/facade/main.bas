IMPORT "library/library.bas"

ACTOR Consumer
    RECEIVE library.Ping(value)
        print($"received={value}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    number = library.identity(42)
    text = library.identity("ok")
    point = library.Point(7)
    ping = library.Ping(7)
    library.announce()
    worker = SPAWN library.Worker
    SEND worker, ping
    print($"{library.answer()}:{number}:{text}:{point.value}:{ping.value}")
    receiver_actor = SPAWN Consumer
    SEND receiver_actor, ping
END ACTION
