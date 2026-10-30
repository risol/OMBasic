IMPORT "@std/encoding.bas"

ACTION main()
    original = "a\u{0}b"
    encoded = encoding.utf8_encode(original)
    SELECT encoded
    CASE Error(problem)
        print($"encode-error={problem.code}")
    CASE Ok(buffer)
        hex = encoding.hex_encode(buffer)
        SELECT hex
        CASE Ok(value)
            print($"utf8-hex={value}")
        CASE Error(problem)
            print($"hex-error={problem.code}")
        END SELECT
        decoded = encoding.utf8_decode(buffer)
        SELECT decoded
        CASE Ok(value)
            reencoded = encoding.utf8_encode(value)
            SELECT reencoded
            CASE Ok(roundtrip_buffer)
                roundtrip_hex = encoding.hex_encode(roundtrip_buffer)
                SELECT roundtrip_hex
                CASE Ok(roundtrip_value)
                    IF roundtrip_value = "610062" THEN
                        print("nul-roundtrip=true")
                    ELSE
                        print("nul-roundtrip=false")
                    END IF
                CASE Error(problem)
                    print($"nul-roundtrip-error={problem.code}")
                END SELECT
            CASE Error(problem)
                print($"nul-roundtrip-error={problem.code}")
            END SELECT
        CASE Error(problem)
            print($"decode-error={problem.code}|{problem.offset}")
        END SELECT
    END SELECT

    international = encoding.utf8_encode("café 東京 😀 \u{10ffff}")
    SELECT international
    CASE Ok(buffer)
        roundtrip = encoding.utf8_decode(buffer)
        SELECT roundtrip
        CASE Ok(value)
            IF value = "café 東京 😀 \u{10ffff}" THEN
                print("international-roundtrip=true")
            ELSE
                print("international-roundtrip=false")
            END IF
        CASE Error(problem)
            print($"international-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"international-error={problem.code}")
    END SELECT

    malformed = encoding.buffer_from_bytes([97, 226, 130])
    SELECT malformed
    CASE Ok(buffer)
        invalid = encoding.utf8_decode(buffer)
        SELECT invalid
        CASE Error(problem)
            print($"invalid={problem.code}|{problem.offset}")
        CASE Ok(value)
            print("invalid=accepted")
        END SELECT
    CASE Error(problem)
        print($"invalid-buffer={problem.code}")
    END SELECT

    overlong = encoding.utf8_decode(encoding.Buffer([192, 128]))
    SELECT overlong
    CASE Error(problem)
        print($"overlong={problem.code}|{problem.offset}")
    CASE Ok(value)
        print("overlong=accepted")
    END SELECT

    surrogate = encoding.utf8_decode(encoding.Buffer([237, 160, 128]))
    SELECT surrogate
    CASE Error(problem)
        print($"surrogate={problem.code}|{problem.offset}")
    CASE Ok(value)
        print("surrogate=accepted")
    END SELECT

    out_of_range = encoding.utf8_decode(encoding.Buffer([244, 144, 128, 128]))
    SELECT out_of_range
    CASE Error(problem)
        print($"out-of-range={problem.code}|{problem.offset}")
    CASE Ok(value)
        print("out-of-range=accepted")
    END SELECT
END ACTION
