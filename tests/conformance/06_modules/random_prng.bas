IMPORT "@std/random/random.bas"

ACTION main()
    first = random.next_sample(random.seed(1))
    second = random.next_sample(first.state)
    print($"first={first.value}")
    print($"second={second.value}")
    repeated = random.next_sample(random.seed(1))
    print($"reproducible={first.value = repeated.value}")
    print($"in-range={first.value >= 1 AND first.value <= 2147483646}")
    negative = random.next_sample(random.seed(-1))
    print($"negative-seed={negative.value}")
END ACTION
