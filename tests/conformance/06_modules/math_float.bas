IMPORT "@std/math/math.bas"

ACTION main()
    print($"sqrt={math.sqrt(9.0)}")
    print($"sin={math.sin(0.0)}")
    print($"round={math.round(-1.5)}")
    print($"domain={math.is_nan(math.sqrt(-1.0))}")
    print($"finite={math.is_finite(math.log(0.0))}")
    print($"trig={math.cos(0.0)}:{math.tan(0.0)}:{math.asin(0.0)}:{math.acos(1.0)}:{math.atan(0.0)}")
    print($"quadrant={math.atan2(0.0, -1.0) > 3.0}")
    print($"exp-log={math.exp(0.0)}:{math.log(1.0)}:{math.log10(100.0)}")
    print($"rounding={math.floor(-1.2)}:{math.ceil(-1.2)}:{math.trunc(-1.2)}")
END ACTION
