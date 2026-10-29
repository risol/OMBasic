IMPORT "library/library.bas"

ACTION main()
    number = library.identity(42)
    text = library.identity("ok")
    point = library.Point(7)
    ping = library.Ping(7)
    library.announce()
    worker = SPAWN library.Worker
    SEND worker, ping
    print($"{library.answer()}:{number}:{text}:{point.value}:{ping.value}")
END ACTION
