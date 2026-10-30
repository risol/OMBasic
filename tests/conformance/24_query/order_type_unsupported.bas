ACTION main()
    values = [1, 2]
    invalid = QUERY item IN values
        ORDER BY item > 1 ASCENDING
        SELECT item
    END QUERY
END ACTION