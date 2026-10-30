IMPORT "@std/csv/csv.bas"
IMPORT "@std/text/text.bas"

ACTION main()
    parsed = csv.parse("name,note\r\nAda,\"says \"\"hello\"\"\"\r\nLin,\"line one\nline two\"")
    SELECT parsed
    CASE Ok(table)
        note = csv.get(table, 1, 1)
        SELECT note
        CASE Ok(value)
            print($"rows={csv.row_count(table)}|fields={csv.field_count(table, 0)}|note={value}")
        CASE Error(problem)
            print($"get-error={problem.code}")
        END SELECT
        multiline = csv.get(table, 2, 1)
        SELECT multiline
        CASE Ok(value)
            print($"multiline={text.find(value, "\n") >= 0}")
        CASE Error(problem)
            print($"get-error={problem.code}")
        END SELECT
        serialized = csv.stringify(table)
        SELECT serialized
        CASE Ok(value)
            print($"roundtrip={value = "name,note\r\nAda,\"says \"\"hello\"\"\"\r\nLin,\"line one\nline two\""}")
        CASE Error(problem)
            print($"stringify-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"parse-error={problem.code}|{problem.byte_offset}")
    END SELECT

    malformed = csv.parse("a,\"bad\"tail")
    SELECT malformed
    CASE Error(problem)
        print($"malformed={problem.code}|{problem.byte_offset}")
    CASE Ok(table)
        print($"unexpected={csv.row_count(table)}")
    END SELECT
END ACTION
