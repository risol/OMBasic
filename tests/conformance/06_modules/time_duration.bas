IMPORT "@std/time/time.bas"

ACTION main()
    first = time.from_milliseconds(1200)
    second = time.from_milliseconds(-200)
    total = time.add(first, second)
    difference = time.subtract(first, second)
    print($"sum={time.to_milliseconds(total)}")
    print($"difference={time.to_milliseconds(difference)}")
    print($"compare={time.compare(first, second)}")
END ACTION
