ACTION main()
    values = [0, 1, 2, 3, 4, 5, 6, 7]
    selected = QUERY item IN values
        WHERE item > 1
        WHERE item < 7
        WHERE item % 2 = 0
        SELECT item * 10
    END QUERY
    print($"selected={selected[0]},{selected[1]}")
END ACTION
