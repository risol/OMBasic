RECORD Pair(left, right)

FUNCTION identity(value)
    RETURN value
END FUNCTION

ACTION main()
    number_value = identity(42)
    text_value = identity("ok")
    pair_value = Pair(number_value, text_value)
    values = [pair_value.left, 5]
    mapping = {"answer": values[0]}
    print($"{mapping["answer"]}:{pair_value.right}")
END ACTION
