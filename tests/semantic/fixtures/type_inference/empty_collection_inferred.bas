FUNCTION empty_value()
    RETURN []
END FUNCTION

ACTION main()
    integers = empty_value() + [7]
    strings = empty_value() + ["value"]
END ACTION
