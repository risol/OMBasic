FUNCTION get_values()
    RETURN [3, 1, 2]
END FUNCTION

ACTION main()
    projected = QUERY item IN get_values()
        WHERE item > 0
        WHERE item < 3
        ORDER BY item DESCENDING
        TAKE 2
        SELECT item
    END QUERY
END ACTION
