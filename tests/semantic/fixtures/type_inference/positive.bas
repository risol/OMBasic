RECORD Pair(left, right)
MESSAGE Ping(value)
MESSAGE Pair_Message(value)

FUNCTION identity(value)
    RETURN value
END FUNCTION

FUNCTION use_identity()
    integer_value = identity(1)
    string_value = identity("ok")
    RETURN string_value
END FUNCTION

FUNCTION has_text(values)
    RETURN values[0] = "ok"
END FUNCTION

FUNCTION has_key(mapping)
    RETURN mapping["key"] = 1
END FUNCTION

FUNCTION unwrap_result(outcome)
    result_value = TRY outcome
    RETURN Ok(result_value)
END FUNCTION

FUNCTION cycle_left(value)
    RETURN cycle_right(value)
END FUNCTION

FUNCTION cycle_right(value)
    RETURN cycle_left(value)
END FUNCTION

FUNCTION choose(flag)
    IF flag THEN
        RETURN 1
    ELSE
        RETURN 2
    END IF
END FUNCTION

FUNCTION make_result(flag)
    IF flag THEN
        RETURN Ok(1)
    ELSE
        RETURN Error("failed")
    END IF
END FUNCTION

ACTION main()
    identity_value = use_identity()
    concrete_result = make_result(TRUE)
    contextual_list = has_text([])
    contextual_map = has_key({})
    unwrapped = unwrap_result(Ok(2))
    recursive_value = cycle_left(3)
    chosen_value = choose(TRUE)
    pair_value = Pair([1], {"key": "one"})
    updated = pair_value WITH left = [2]
    values = [1, 2]
    first = values[0]
    mapping = {"key": 3}
    mapped = mapping["key"]
    outcome = Ok(1)
    SELECT outcome
    CASE Ok(value)
        selected = value
    CASE Error(problem)
        selected_error = problem
    END SELECT
    worker_ref = SPAWN Worker
    SEND worker_ref, Ping(1)
    SEND worker_ref, Pair_Message(pair_value)
END ACTION

ACTOR Worker
    VAR count = 0
    VAR initial_count = count + 1
    RECEIVE Ping(value)
        count = count + value + initial_count
    END RECEIVE
    RECEIVE Pair_Message(value)
    END RECEIVE
END ACTOR
