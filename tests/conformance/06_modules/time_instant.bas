IMPORT "@std/time/instant.bas"

ACTION main()
    earlier = instant.instant_from_milliseconds(1200)
    later = instant.instant_from_milliseconds(1700)
    print($"instant={instant.instant_to_milliseconds(earlier)}|{instant.instant_compare(earlier, later)}|{instant.instant_difference_milliseconds(earlier, later)}")
END ACTION
