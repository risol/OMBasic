ACTION main()
    values = [1, 2]
    invalid = QUERY item IN values
        TAKE 1.0
        SELECT item
    END QUERY
END ACTION