IMPORT "@std/collections/list.bas"

ACTION main()
    values = [1, 2, 3, 4]
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
    print($"selected={selected[0]}|empty={list.length(empty)}|order={ordered[0]},{ordered[1]},{ordered[2]},{ordered[3]}|count={list.length(ordered)}")
END ACTION
