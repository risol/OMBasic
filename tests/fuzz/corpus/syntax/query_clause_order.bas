ACTION malformed_query_seed(users)
    output = QUERY item IN users
        SELECT item
        WHERE item > 0
    END QUERY
END ACTION