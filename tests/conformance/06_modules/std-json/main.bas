IMPORT "@std/json/json.bas"

ACTION main()
    SELECT json.parse(" { \"large\" : 123456789012345678901234567890.00e+0, \"items\" : [true, null, \"é\"] } ")
    CASE Ok(document)
        SELECT json.stringify(document)
        CASE Ok(compact)
            print(compact)
        CASE Error(problem)
            print("stringify failed")
        END SELECT
        SELECT json.kind(document)
        CASE Ok(kind_name)
            print(kind_name)
        CASE Error(problem)
            print("kind failed")
        END SELECT
        SELECT json.object_get(document, "items")
        CASE Ok(items)
            SELECT json.array_length(items)
            CASE Ok(count)
                print($"items={count}")
            CASE Error(problem)
                print("length failed")
            END SELECT
            SELECT json.array_get(items, 2)
            CASE Ok(third)
                SELECT json.stringify(third)
                CASE Ok(encoded)
                    print(encoded)
                CASE Error(problem)
                    print("item stringify failed")
                END SELECT
            CASE Error(problem)
                print("array get failed")
            END SELECT
        CASE Error(problem)
            print("object get failed")
        END SELECT
    CASE Error(problem)
        print($"valid parse failed={problem.code}")
    END SELECT

    SELECT json.parse("{\"a\":1,\"\\u0061\":2}")
    CASE Ok(document)
        print("duplicate key accepted")
    CASE Error(problem)
        print($"duplicate={problem.code}")
    END SELECT

    SELECT json.parse("01")
    CASE Ok(document)
        print("invalid number accepted")
    CASE Error(problem)
        print($"invalid={problem.code}:{problem.byte_offset}")
    END SELECT

    SELECT json.number("1.2300e+5")
    CASE Ok(number_value)
        SELECT json.object([json.Entry("label", json.string_value("café")),
                            json.Entry("n", number_value)])
        CASE Ok(built_object)
            SELECT json.stringify(built_object)
            CASE Ok(built_text)
                print(built_text)
            CASE Error(problem)
                print("object stringify failed")
            END SELECT
        CASE Error(problem)
            print("object construction failed")
        END SELECT
    CASE Error(problem)
        print("number construction failed")
    END SELECT

    SELECT json.parse("{}")
    CASE Ok(empty_object)
        SELECT json.stringify(empty_object)
        CASE Ok(encoded_empty_object)
            print(encoded_empty_object)
        CASE Error(problem)
            print("empty object stringify failed")
        END SELECT
    CASE Error(problem)
        print("empty object parse failed")
    END SELECT

    SELECT json.parse("[]")
    CASE Ok(empty_array)
        SELECT json.stringify(empty_array)
        CASE Ok(encoded_empty_array)
            print(encoded_empty_array)
        CASE Error(problem)
            print("empty array stringify failed")
        END SELECT
    CASE Error(problem)
        print("empty array parse failed")
    END SELECT
END ACTION
