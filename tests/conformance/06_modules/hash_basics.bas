IMPORT "@std/hash/hash.bas"
IMPORT "@std/encoding.bas"

ACTION main()
    empty = hash.sha256_hex(encoding.Buffer([]))
    SELECT empty
    CASE Ok(value)
        print($"sha256-empty={value}")
    CASE Error(problem)
        print($"sha256-empty-error={problem.code}")
    END SELECT

    abc = hash.sha256_hex(encoding.Buffer([97, 98, 99]))
    SELECT abc
    CASE Ok(value)
        print($"sha256-abc={value}")
    CASE Error(problem)
        print($"sha256-abc-error={problem.code}")
    END SELECT

    block = hash.sha256_hex(encoding.Buffer([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63]))
    SELECT block
    CASE Ok(value)
        print($"sha256-block={value}")
    CASE Error(problem)
        print($"sha256-block-error={problem.code}")
    END SELECT

    digest = hash.sha256(encoding.Buffer([0, 255, 0]))
    encoded = encoding.hex_encode(digest)
    SELECT encoded
    CASE Ok(hex)
        print($"sha256-binary={hex}")
    CASE Error(problem)
        print($"sha256-binary-error={problem.code}")
    END SELECT

    crc = hash.crc32(encoding.Buffer([49, 50, 51, 52, 53, 54, 55, 56, 57]))
    SELECT crc
    CASE Ok(value)
        print($"crc32={value}")
    CASE Error(problem)
        print($"crc32-error={problem.code}")
    END SELECT
END ACTION
