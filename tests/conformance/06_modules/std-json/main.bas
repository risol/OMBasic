IMPORT "@std/json/json.bas"

ACTION main()
    SELECT json.parse(" { \"large\" : 123456789012345678901234567890.00e+0, \"items\" : [true, null, \"é\"] } ")
    CASE Ok(document)
        SELECT json.object_length(document)
        CASE Ok(count)
            print($"object-count={count}")
        CASE Error(problem)
            print("object length failed")
        END SELECT
        SELECT json.object_keys(document)
        CASE Ok(keys)
            print($"keys={keys[0]}:{keys[1]}")
        CASE Error(problem)
            print("object keys failed")
        END SELECT
        SELECT json.object_entries(document)
        CASE Ok(entries)
            print($"entries={entries[0].key}:{entries[0].value.encoded}|{entries[1].key}:{entries[1].value.encoded}")
        CASE Error(problem)
            print("object entries failed")
        END SELECT
        SELECT json.object_has(document, "items")
        CASE Ok(found)
            print($"has-items={found}")
        CASE Error(problem)
            print("object has failed")
        END SELECT
        SELECT json.object_has(document, "missing")
        CASE Ok(found)
            print($"has-missing={found}")
        CASE Error(problem)
            print("object has missing failed")
        END SELECT
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
        SELECT json.object_entries(empty_object)
        CASE Ok(empty_entries)
            VAR empty_entries_count = 0
            FOR entry IN empty_entries
                empty_entries_count = empty_entries_count + 1
            END FOR
            print($"empty-entries={empty_entries_count}")
        CASE Error(problem)
            print("empty object entries failed")
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

    SELECT json.parse("\"line\\n\\u00e9\"")
    CASE Ok(string_item)
        SELECT json.as_string(string_item)
        CASE Ok(decoded)
            print($"decoded={decoded}")
        CASE Error(problem)
            print("string accessor failed")
        END SELECT
    CASE Error(problem)
        print("string parse failed")
    END SELECT

    SELECT json.parse("true")
    CASE Ok(boolean_item)
        SELECT json.as_boolean(boolean_item)
        CASE Ok(flag)
            print($"boolean={flag}")
        CASE Error(problem)
            print("boolean accessor failed")
        END SELECT
    CASE Error(problem)
        print("boolean parse failed")
    END SELECT

    SELECT json.parse("null")
    CASE Ok(null_item)
        SELECT json.is_null(null_item)
        CASE Ok(flag)
            print($"is-null={flag}")
        CASE Error(problem)
            print("null accessor failed")
        END SELECT
    CASE Error(problem)
        print("null parse failed")
    END SELECT

    SELECT json.parse("{\"a\":1,\"\\u0062\":2}")
    CASE Ok(key_document)
        SELECT json.object_keys(key_document)
        CASE Ok(keys)
            print($"decoded-keys={keys[0]}:{keys[1]}")
        CASE Error(problem)
            print("decoded keys failed")
        END SELECT
    CASE Error(problem)
        print("key object parse failed")
    END SELECT

    SELECT json.parse("9223372036854775807")
    CASE Ok(integer_item)
        SELECT json.as_int64(integer_item)
        CASE Ok(integer_value)
            print($"int64={integer_value}")
        CASE Error(problem)
            print("int64 accessor failed")
        END SELECT
    CASE Error(problem)
        print("int64 parse failed")
    END SELECT

    SELECT json.parse("1.0")
    CASE Ok(fractional_item)
        SELECT json.as_int64(fractional_item)
        CASE Ok(integer_value)
            print("fraction accepted")
        CASE Error(problem)
            print($"fraction-rejected={problem.code}")
        END SELECT
    CASE Error(problem)
        print("fraction parse failed")
    END SELECT

    SELECT json.parse("1.25e2")
    CASE Ok(float_item)
        SELECT json.as_float64(float_item)
        CASE Ok(float_value)
            print($"float64={float_value}")
        CASE Error(problem)
            print("float64 accessor failed")
        END SELECT
    CASE Error(problem)
        print("float64 parse failed")
    END SELECT

    SELECT json.parse("1e9999")
    CASE Ok(overflow_item)
        SELECT json.as_float64(overflow_item)
        CASE Ok(float_value)
            print("float64 overflow accepted")
        CASE Error(problem)
            print($"float64-overflow={problem.code}")
        END SELECT
    CASE Error(problem)
        print("float64 overflow parse failed")
    END SELECT

    SELECT json.parse("1e-9999")
    CASE Ok(underflow_item)
        SELECT json.as_float64(underflow_item)
        CASE Ok(float_value)
            print("float64 underflow accepted")
        CASE Error(problem)
            print($"float64-underflow={problem.code}")
        END SELECT
    CASE Error(problem)
        print("float64 underflow parse failed")
    END SELECT
END ACTION
