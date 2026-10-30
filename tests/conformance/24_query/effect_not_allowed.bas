ACTION main()
    source_rows = ["x"]
    output = QUERY item IN source_rows
        SELECT print(item)
    END QUERY
END ACTION



