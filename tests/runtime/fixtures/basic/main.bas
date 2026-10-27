RECORD Pair(left, right)
MESSAGE Notice(value)

FUNCTION add(left, right)
    RETURN left + right
END FUNCTION

FUNCTION invoke_callback(operation, value)
    RETURN operation(value)
END FUNCTION

FUNCTION triple(value)
    RETURN value * 3
END FUNCTION

FUNCTION unwrap_plus(outcome, amount)
    value = TRY outcome
    RETURN Ok(value + amount)
END FUNCTION

FUNCTION recursive_sum(value)
    IF value <= 0 THEN
        RETURN 0
    ELSE
        RETURN value + recursive_sum(value - 1)
    END IF
END FUNCTION

FUNCTION run()
    instance = Pair(2, 3)
    updated = instance WITH right = 5
    values = [updated.left, updated.right]
    VAR total = 0
    FOR index = 1 TO 3
        total = total + index
    END FOR
    FOR element IN values
        total = total + element
    END FOR
    mapping = {"sum": total}
    RETURN unwrap_plus(Ok(mapping["sum"] + recursive_sum(4)), 1)
END FUNCTION

FUNCTION propagate_error()
    RETURN unwrap_plus(Error(7), 1)
END FUNCTION

FUNCTION choose_error(outcome)
    SELECT outcome
    CASE Ok(value)
        RETURN 0
    CASE Error(problem)
        RETURN problem
    END SELECT
    RETURN 0
END FUNCTION

FUNCTION unwrap_error_pattern()
    RETURN choose_error(Error(9))
END FUNCTION

FUNCTION make_message()
    RETURN Notice(12)
END FUNCTION

FUNCTION render()
    RETURN $"value {add(1, 2)}"
END FUNCTION

FUNCTION indirect_call()
    RETURN invoke_callback(triple, 7)
END FUNCTION

FUNCTION float_math()
    RETURN (1.5 + 2.25) * 2.0
END FUNCTION

FUNCTION divide_by_zero()
    RETURN 1 / 0
END FUNCTION

FUNCTION overflow_int()
    RETURN 9223372036854775807 + 1
END FUNCTION
