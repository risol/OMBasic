IMPORT "@std/math/math.bas"

ACTION main()
    sum = math.add(20, 22)
    magnitude = math.abs(-7)
    lower = math.min(sum, magnitude)
    upper = math.max(sum, magnitude)
    bounded = math.clamp(100, 0, 10)
    invalid_bounds = math.clamp(5, 10, 0)
    print($"{sum},{magnitude},{lower},{upper},{bounded},{invalid_bounds}")
END ACTION
