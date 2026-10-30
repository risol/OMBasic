IMPORT "@std/compression/compression.bas"
IMPORT "@std/collections/list.bas"
IMPORT "@std/encoding.bas"

ACTION main()
    source = encoding.Buffer([0, 1, 2, 255, 104, 101, 108, 108, 111])
    zipped = compression.gzip_compress(source)
    SELECT zipped
    CASE Ok(gzip_bytes)
        gunzipped = compression.gzip_decompress(gzip_bytes)
        SELECT gunzipped
        CASE Ok(output)
            same = list.length(output.bytes) = list.length(source.bytes) AND output.bytes[0] = source.bytes[0] AND output.bytes[1] = source.bytes[1] AND output.bytes[2] = source.bytes[2] AND output.bytes[3] = source.bytes[3] AND output.bytes[4] = source.bytes[4] AND output.bytes[5] = source.bytes[5] AND output.bytes[6] = source.bytes[6] AND output.bytes[7] = source.bytes[7] AND output.bytes[8] = source.bytes[8]
            print($"gzip={gzip_bytes.bytes[0]}|{gzip_bytes.bytes[1]}|{same}")
        CASE Error(problem)
            print($"gzip-decode-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"gzip-encode-error={problem.code}")
    END SELECT

    deflated = compression.deflate_compress(source)
    SELECT deflated
    CASE Ok(deflate_bytes)
        inflated = compression.deflate_decompress(deflate_bytes)
        SELECT inflated
        CASE Ok(output)
            same = list.length(output.bytes) = list.length(source.bytes) AND output.bytes[0] = source.bytes[0] AND output.bytes[1] = source.bytes[1] AND output.bytes[2] = source.bytes[2] AND output.bytes[3] = source.bytes[3] AND output.bytes[4] = source.bytes[4] AND output.bytes[5] = source.bytes[5] AND output.bytes[6] = source.bytes[6] AND output.bytes[7] = source.bytes[7] AND output.bytes[8] = source.bytes[8]
            print($"deflate={deflate_bytes.bytes[0]}|{deflate_bytes.bytes[1]}|{same}")
        CASE Error(problem)
            print($"deflate-decode-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"deflate-encode-error={problem.code}")
    END SELECT

    known_gzip = encoding.Buffer([31, 139, 8, 0, 0, 0, 0, 0, 2, 255, 203, 72, 205, 201, 201, 7, 0, 134, 166, 16, 54, 5, 0, 0, 0])
    decoded_gzip = compression.gzip_decompress(known_gzip)
    SELECT decoded_gzip
    CASE Ok(output)
        print($"known-gzip={output.bytes[0]}|{output.bytes[4]}")
    CASE Error(problem)
        print($"known-gzip-error={problem.code}")
    END SELECT

    gzip_optional = encoding.Buffer([31, 139, 8, 30, 0, 0, 0, 0, 0, 255, 2, 0, 88, 89, 102, 105, 108, 101, 46, 116, 120, 116, 0, 116, 101, 115, 116, 0, 134, 56, 203, 72, 205, 201, 201, 7, 0, 134, 166, 16, 54, 5, 0, 0, 0])
    optional_result = compression.gzip_decompress(gzip_optional)
    SELECT optional_result
    CASE Ok(output)
        print($"gzip-optional={output.bytes[0]}|{output.bytes[4]}")
    CASE Error(problem)
        print($"gzip-optional-error={problem.code}")
    END SELECT

    empty = compression.gzip_compress(encoding.Buffer([]))
    SELECT empty
    CASE Ok(empty_gzip)
        empty_output = compression.gzip_decompress(empty_gzip)
        SELECT empty_output
        CASE Ok(output)
            print($"gzip-empty={list.length(output.bytes)}")
        CASE Error(problem)
            print($"gzip-empty-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"gzip-empty-encode-error={problem.code}")
    END SELECT

    corrupt_gzip = encoding.Buffer([31, 139, 8, 0, 0, 0, 0, 0, 2, 255, 203, 72, 205, 201, 201, 7, 0, 134, 166, 16, 54, 5, 0, 0, 1])
    corrupt = compression.gzip_decompress(corrupt_gzip)
    SELECT corrupt
    CASE Error(problem)
        print($"checksum={problem.code}")
    CASE Ok(output)
        print("checksum=unexpected-success")
    END SELECT

    malformed = compression.deflate_decompress(encoding.Buffer([1, 2, 3]))
    SELECT malformed
    CASE Error(problem)
        print($"malformed={problem.code}")
    CASE Ok(output)
        print("malformed=unexpected-success")
    END SELECT
END ACTION
