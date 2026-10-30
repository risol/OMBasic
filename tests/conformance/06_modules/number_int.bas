IMPORT "@std/number/number.bas"

ACTION main()
    parsed = number.parse_int("-9223372036854775808")
    SELECT parsed
    CASE Ok(value)
        print(number.format_int(value))
    CASE Error(problem)
        print(problem.code)
    END SELECT

    radix_value = number.parse_int_radix("7fffffff", 16)
    SELECT radix_value
    CASE Ok(value)
        formatted = number.format_int_radix(value, 2)
        SELECT formatted
        CASE Ok(text)
            print(text)
        CASE Error(problem)
            print(problem.code)
        END SELECT
    CASE Error(problem)
        print(problem.code)
    END SELECT

    invalid = number.parse_int("12x")
    SELECT invalid
    CASE Ok(value)
        print("unexpected")
    CASE Error(problem)
        print($"{problem.code}|{problem.byte_offset}")
    END SELECT

    bool_value = number.parse_bool("true")
    SELECT bool_value
    CASE Ok(value)
        print(number.format_bool(value))
    CASE Error(problem)
        print(problem.code)
    END SELECT

    decimal = number.parse_float("1.25e2")
    SELECT decimal
    CASE Ok(value)
        rendered = number.format_float(value)
        SELECT rendered
        CASE Ok(text)
            print(text)
        CASE Error(problem)
            print(problem.code)
        END SELECT
    CASE Error(problem)
        print(problem.code)
    END SELECT

    tenth = number.parse_float("0.1")
    SELECT tenth
    CASE Ok(value)
        rendered = number.format_float(value)
        SELECT rendered
        CASE Ok(text)
            print(text)
        CASE Error(problem)
            print(problem.code)
        END SELECT
    CASE Error(problem)
        print(problem.code)
    END SELECT

    invalid_float = number.parse_float("1e9999")
    SELECT invalid_float
    CASE Ok(value)
        print("unexpected-float")
    CASE Error(problem)
        print(problem.code)
    END SELECT

    non_finite = number.parse_float("-Infinity")
    SELECT non_finite
    CASE Ok(value)
        print("unexpected-infinity")
    CASE Error(problem)
        print(problem.code)
    END SELECT

    negative_zero = number.parse_float("-0.0")
    SELECT negative_zero
    CASE Ok(value)
        rendered = number.format_float(value)
        SELECT rendered
        CASE Ok(text)
            print(text)
        CASE Error(problem)
            print(problem.code)
        END SELECT
    CASE Error(problem)
        print(problem.code)
    END SELECT

    subnormal = number.parse_float("4.9406564584124654e-324")
    SELECT subnormal
    CASE Ok(value)
        rendered = number.format_float(value)
        SELECT rendered
        CASE Ok(text)
            print(text)
        CASE Error(problem)
            print(problem.code)
        END SELECT
    CASE Error(problem)
        print(problem.code)
    END SELECT
END ACTION
