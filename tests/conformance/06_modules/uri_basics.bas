IMPORT "@std/uri/uri.bas"
IMPORT "@std/collections/list.bas"

ACTION main()
    parsed = uri.parse("https://user:pass@example.com:8443/a/b?x=1#frag")
    SELECT parsed
    CASE Error(problem)
        print($"parse-error={problem.code}|{problem.byte_offset}")
    CASE Ok(value)
        formatted = uri.format(value)
        print($"roundtrip={formatted}")
        print($"absolute={uri.is_absolute("https://example.com")}")
        print($"default-port={uri.default_port("HTTPS")}")
    END SELECT

    encoded = uri.percent_encode("café 東京", "path")
    SELECT encoded
    CASE Ok(value)
        print($"encoded={value}")
        decoded = uri.percent_decode(value)
        SELECT decoded
        CASE Ok(text)
            print($"decoded={text}")
        CASE Error(problem)
            print($"decode-error={problem.code}|{problem.byte_offset}")
        END SELECT
    CASE Error(problem)
        print($"encode-error={problem.code}|{problem.byte_offset}")
    END SELECT

    query = uri.query_encode([uri.Query_Pair("a&b", "x=y"),
        uri.Query_Pair("empty", "")])
    SELECT query
    CASE Ok(value)
        print($"query={value}")
        pairs = uri.query_decode(value)
        SELECT pairs
        CASE Ok(decoded_pairs)
            print($"query-count={list.length(decoded_pairs)}")
        CASE Error(problem)
            print($"query-error={problem.code}|{problem.byte_offset}")
        END SELECT
    CASE Error(problem)
        print($"query-encode-error={problem.code}|{problem.byte_offset}")
    END SELECT

    base_result = uri.parse("http://a/b/c/d;p?q")
    reference_result = uri.parse("../g")
    SELECT base_result
    CASE Ok(base)
        SELECT reference_result
        CASE Ok(reference)
            resolved = uri.resolve_reference(base, reference)
            print($"resolved={uri.format(resolved)}")
        CASE Error(problem)
            print($"reference-error={problem.code}")
        END SELECT
    CASE Error(problem)
        print($"base-error={problem.code}")
    END SELECT

    ipv6 = uri.parse("http://[2001:db8::1]:443/")
    SELECT ipv6
    CASE Ok(value)
        print($"ipv6={uri.format(value)}")
    CASE Error(problem)
        print($"ipv6-error={problem.code}")
    END SELECT

    empty_delimiters = uri.parse("mailto:user@example.com?#")
    SELECT empty_delimiters
    CASE Ok(value)
        print($"empty-delimiters={value.query_present}|{value.fragment_present}|{uri.format(value)}")
    CASE Error(problem)
        print($"empty-delimiters-error={problem.code}")
    END SELECT

    malformed = uri.percent_decode("%G0")
    SELECT malformed
    CASE Error(problem)
        print($"malformed={problem.code}|{problem.byte_offset}")
    CASE Ok(value)
        print($"malformed-accepted={value}")
    END SELECT

    print($"dot-segments={uri.remove_dot_segments("/a/b/../c/./")}")

    invalid_uri = uri.parse("http://example.com/%Q0")
    SELECT invalid_uri
    CASE Error(problem)
        print($"invalid-uri={problem.code}|{problem.byte_offset}")
    CASE Ok(value)
        print($"invalid-uri-accepted={uri.format(value)}")
    END SELECT
END ACTION
