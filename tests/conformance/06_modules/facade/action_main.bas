IMPORT "library/library.bas"

ACTION main()
    number = library.identity(42)
    point = library.Point(number)
    ping = library.Ping(point.value)
    library.announce()
END ACTION
