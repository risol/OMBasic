IMPORT "@std/math/math.bas"

ACTION main()
    print($"{math.sign(-5)}|{math.sign(0)}|{math.sign(8)}")
    print($"{math.is_even(-9223372036854775808)}|{math.is_odd(-5)}|{math.is_even(7)}")
END ACTION
