IMPORT "@std/collections/list.bas"

ACTION main()
    values = [1, 2, 3, 4]
    identity = QUERY item IN values
        SELECT item
    END QUERY
    selected = QUERY item IN values
        WHERE item > 1
        WHERE item < 4
        TAKE 1
        SELECT item * 2
    END QUERY
    empty = QUERY item IN values
        TAKE 0
        SELECT item
    END QUERY
    ordered = QUERY item IN [4, 2, 3, 2, 1]
        ORDER BY item % 2 ASCENDING, item DESCENDING
        TAKE 4
        SELECT item
    END QUERY
    projected = QUERY item IN [0, 1, 2]
        ORDER BY item DESCENDING
        TAKE 1
        SELECT 10 / item
    END QUERY
    print($"identity={identity[0]}|selected={selected[0]}|empty={list.length(empty)}|order={ordered[0]},{ordered[1]},{ordered[2]},{ordered[3]}|count={list.length(ordered)}|projected={projected[0]}")
END ACTION
