IMPORT "@std/uuid/uuid.bas"
IMPORT "@std/encoding.bas"

ACTION main()
    source = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
    created = uuid.v4_from_random_bytes(encoding.Buffer(source))
    SELECT created
    CASE Ok(value)
        print(value)
        parsed = uuid.parse(value)
        SELECT parsed
        CASE Ok(buffer)
            formatted = uuid.format(buffer)
            SELECT formatted
            CASE Ok(text)
                print($"roundtrip={text}")
            CASE Error(problem)
                print($"format-error={problem.code}")
            END SELECT
        CASE Error(problem)
            print($"parse-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"unexpected_error={problem.code}")
    END SELECT

    invalid = uuid.v4_from_random_bytes(encoding.Buffer([1, 2, 3]))
    SELECT invalid
    CASE Error(problem)
        print(problem.code)
    CASE Ok(value)
        print("unexpected")
    END SELECT

    malformed = uuid.parse("00010203-0405-4607-8809-0a0b0c0d0e0g")
    SELECT malformed
    CASE Error(problem)
        print($"malformed={problem.code}")
    CASE Ok(value)
        print("malformed-accepted")
    END SELECT
END ACTION
