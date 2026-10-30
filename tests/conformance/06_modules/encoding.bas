IMPORT "@std/encoding.bas"

ACTION main()
    created = encoding.buffer_from_bytes([102, 111, 111])
    SELECT created
    CASE Error(problem)
        print($"buffer-error={problem.code}")
    CASE Ok(bytes)
        hex_result = encoding.hex_encode(bytes)
        SELECT hex_result
        CASE Ok(encoded_hex)
            print($"hex={encoded_hex}")
        CASE Error(problem)
            print($"hex-error={problem.code}")
        END SELECT
        base64_result = encoding.base64_encode(bytes)
        SELECT base64_result
        CASE Ok(encoded_base64)
            print($"base64={encoded_base64}")
            decoded = encoding.base64_decode(encoded_base64)
            SELECT decoded
            CASE Ok(decoded_buffer)
                roundtrip_result = encoding.hex_encode(decoded_buffer)
                SELECT roundtrip_result
                CASE Ok(roundtrip_hex)
                    print($"roundtrip={roundtrip_hex}")
                CASE Error(roundtrip_error)
                    print($"roundtrip-error={roundtrip_error.code}")
                END SELECT
            CASE Error(decode_error)
                print($"decode-error={decode_error.code}")
            END SELECT
        CASE Error(problem)
            print($"base64-error={problem.code}")
        END SELECT
    END SELECT

    hex_decoded = encoding.hex_decode("00aF")
    SELECT hex_decoded
    CASE Ok(decoded_hex)
        print($"hex-decode={decoded_hex.bytes[0]}|{decoded_hex.bytes[1]}")
    CASE Error(problem)
        print($"hex-decode-error={problem.code}|{problem.offset}")
    END SELECT

    invalid_hex = encoding.hex_decode("0x")
    SELECT invalid_hex
    CASE Ok(unexpected)
        print("invalid-hex=accepted")
    CASE Error(problem)
        print($"invalid-hex={problem.code}|{problem.offset}")
    END SELECT

    invalid_base64 = encoding.base64_decode("Zh==")
    SELECT invalid_base64
    CASE Ok(unexpected)
        print("invalid-base64=accepted")
    CASE Error(problem)
        print($"invalid-base64={problem.code}|{problem.offset}")
    END SELECT

    one_byte = encoding.base64_decode("Zg==")
    SELECT one_byte
    CASE Ok(one_byte_buffer)
        print($"one-byte={one_byte_buffer.bytes[0]}")
    CASE Error(problem)
        print($"one-byte-error={problem.code}")
    END SELECT

    two_bytes = encoding.base64_decode("Zm8=")
    SELECT two_bytes
    CASE Ok(two_byte_buffer)
        print($"two-bytes={two_byte_buffer.bytes[0]}|{two_byte_buffer.bytes[1]}")
    CASE Error(problem)
        print($"two-bytes-error={problem.code}")
    END SELECT

    empty_bytes = encoding.base64_decode("")
    SELECT empty_bytes
    CASE Ok(empty_buffer)
        empty_length = encoding.buffer_length(empty_buffer)
        SELECT empty_length
        CASE Ok(value)
            print($"empty={value}")
        CASE Error(problem)
            print($"empty-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"empty-error={problem.code}")
    END SELECT

    one_source = encoding.buffer_from_bytes([102])
    SELECT one_source
    CASE Ok(one_source_buffer)
        one_encoded = encoding.base64_encode(one_source_buffer)
        SELECT one_encoded
        CASE Ok(value)
            print($"one-encoded={value}")
        CASE Error(problem)
            print($"one-encoded-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"one-source-error={problem.code}")
    END SELECT

    two_source = encoding.buffer_from_bytes([102, 111])
    SELECT two_source
    CASE Ok(two_source_buffer)
        two_encoded = encoding.base64_encode(two_source_buffer)
        SELECT two_encoded
        CASE Ok(value)
            print($"two-encoded={value}")
        CASE Error(problem)
            print($"two-encoded-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"two-source-error={problem.code}")
    END SELECT

    empty_source = encoding.buffer_from_bytes([])
    SELECT empty_source
    CASE Ok(empty_source_buffer)
        empty_encoded = encoding.base64_encode(empty_source_buffer)
        SELECT empty_encoded
        CASE Ok(value)
            print($"empty-encoded={value}")
        CASE Error(problem)
            print($"empty-encoded-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"empty-source-error={problem.code}")
    END SELECT

    invalid_base64_length = encoding.base64_decode("A")
    SELECT invalid_base64_length
    CASE Ok(unexpected)
        print("invalid-base64-length=accepted")
    CASE Error(problem)
        print($"invalid-base64-length={problem.code}|{problem.offset}")
    END SELECT

    invalid_base64_padding = encoding.base64_decode("Zm=9")
    SELECT invalid_base64_padding
    CASE Ok(unexpected)
        print("invalid-base64-padding=accepted")
    CASE Error(problem)
        print($"invalid-base64-padding={problem.code}|{problem.offset}")
    END SELECT

    invalid_base64_character = encoding.base64_decode("Zm?9")
    SELECT invalid_base64_character
    CASE Ok(unexpected)
        print("invalid-base64-character=accepted")
    CASE Error(problem)
        print($"invalid-base64-character={problem.code}|{problem.offset}")
    END SELECT

    multibyte_hex = encoding.hex_decode("00é")
    SELECT multibyte_hex
    CASE Ok(unexpected)
        print("multibyte-hex=accepted")
    CASE Error(problem)
        print($"multibyte-hex={problem.code}|{problem.offset}")
    END SELECT

    multibyte_base64 = encoding.base64_decode("AAAAé")
    SELECT multibyte_base64
    CASE Ok(unexpected)
        print("multibyte-base64=accepted")
    CASE Error(problem)
        print($"multibyte-base64={problem.code}|{problem.offset}")
    END SELECT

    invalid_byte = encoding.buffer_from_bytes([256])
    SELECT invalid_byte
    CASE Ok(unexpected)
        print("invalid-byte=accepted")
    CASE Error(problem)
        print($"invalid-byte={problem.code}|{problem.offset}")
    END SELECT

    negative_byte = encoding.buffer_from_bytes([0, -1])
    SELECT negative_byte
    CASE Ok(unexpected)
        print("negative-byte=accepted")
    CASE Error(problem)
        print($"negative-byte={problem.code}|{problem.offset}")
    END SELECT

    forged_buffer = encoding.Buffer([256 + 0])
    invalid_buffer = encoding.hex_encode(forged_buffer)
    SELECT invalid_buffer
    CASE Ok(unexpected)
        print("forged-buffer=accepted")
    CASE Error(problem)
        print($"forged-buffer={problem.code}")
    END SELECT
END ACTION
