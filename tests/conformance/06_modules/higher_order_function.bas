FUNCTION apply(transform, value)
    RETURN transform(value)
END FUNCTION

FUNCTION add_one(value)
    RETURN value + 1
END FUNCTION

FUNCTION has_text(value)
    RETURN value = "ok"
END FUNCTION

ACTION main()
    print($"int={apply(add_one, 41)}")
    print($"text={apply(has_text, "ok")}")
END ACTION
