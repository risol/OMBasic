RECORD Pair(left, right)
MESSAGE Ping(value)

FUNCTION add(left, right)
    RETURN left + right
END FUNCTION

FUNCTION plus_one(value)
    RETURN value + 1
END FUNCTION

FUNCTION apply_unary(callback, value)
    RETURN callback(value)
END FUNCTION

FUNCTION choose(flag, left, right)
    VAR value = 0
    IF flag THEN
        value = left
    ELSE
        value = right
    END IF
    RETURN value
END FUNCTION

FUNCTION early_return(flag)
    IF flag THEN
        RETURN 1
    END IF
    RETURN 2
END FUNCTION

FUNCTION both_return(flag)
    IF flag THEN
        RETURN 1
    ELSE
        RETURN 2
    END IF
END FUNCTION

FUNCTION sum_to(limit)
    VAR index = 0
    VAR total = 0
    WHILE index < limit
        total = total + index
        index = index + 1
    END WHILE
    RETURN total
END FUNCTION

FUNCTION sum_range(limit)
    VAR total = 0
    FOR index = 1 TO limit
        total = total + index
    END FOR
    RETURN total
END FUNCTION

FUNCTION count_down()
    VAR total = 0
    FOR index = 5 TO 1 STEP -2
        total = total + index
    END FOR
    RETURN total
END FUNCTION

FUNCTION sum_by(limit, stride)
    VAR total = 0
    FOR index = 1 TO limit STEP stride
        total = total + index
    END FOR
    RETURN total
END FUNCTION

FUNCTION make_result(flag)
    IF flag THEN
        RETURN Ok(7)
    ELSE
        RETURN Error("bad")
    END IF
END FUNCTION

FUNCTION unwrap_result(outcome)
    value = TRY outcome
    RETURN Ok(value)
END FUNCTION

ACTION main()
    greeting = "Hello, OMBasic"
    native_sum = ffi_add(20, 22)
    boolean_and = FALSE AND TRUE
    boolean_or = TRUE OR FALSE
    escaped = "line\nfeed \u{1F600}"
    added = add(20, 22)
    indirect_added = apply_unary(plus_one, 41)
    formatted = $"Hello {{world}}: {added}!"
    selected = choose(TRUE, added, 0)
    early = early_return(TRUE)
    either = both_return(FALSE)
    total = sum_to(10)
    ranged_total = sum_range(10)
    descending_total = count_down()
    stepped_total = sum_by(10, 2)
    numbers = [1, 2, 3]
    first_number = numbers[0]
    VAR each_total = 0
    FOR item IN numbers
        IF item = 2 THEN
            CONTINUE
        END IF
        IF item = 3 THEN
            BREAK
        END IF
        each_total = each_total + item
    END FOR
    lookup = {"name": "Alice", "city": "Paris"}
    lookup_name = lookup["name"]
    VAR lookup_key_count = 0
    FOR key IN lookup
        lookup_key_count = lookup_key_count + 1
    END FOR
    pair_value = Pair([1], {"key": "one"})
    pair_left = pair_value.left
    updated_pair = pair_value WITH left = [2]
    updated_right = updated_pair.right
    ping_value = Ping(pair_left)
    null_value = NULL
    null_text = $"value={null_value}"
    null_equal = null_value = NULL
    wrapped = make_result(TRUE)
    unwrapped = unwrap_result(wrapped)
    SELECT wrapped
    CASE Ok(selected_value)
        chosen_value = selected_value
    CASE Error(problem)
        selected_error = problem
    CASE ELSE
        fallback_value = 0
    END SELECT
    SELECT null_value
    CASE NULL
        null_selected = TRUE
    CASE ELSE
        null_selected = FALSE
    END SELECT
END ACTION
