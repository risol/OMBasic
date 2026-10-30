ACTION main()
    values = [1, 2]
    invalid = QUERY item IN values
        WHERE item
        SELECT item
    END QUERY
END ACTION