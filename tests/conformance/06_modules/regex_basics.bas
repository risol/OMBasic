IMPORT "@std/regex/regex.bas"

ACTION main()
    literal = regex.find("cat", "a cat!")
    SELECT literal
    CASE Ok(match)
        print($"literal={match.matched}|{match.start}|{match.end_index}")
    CASE Error(problem)
        print($"literal-error={problem.code}")
    END SELECT

    syntax = regex.find("^(ab|cd)+[0-9]$", "abcd7")
    SELECT syntax
    CASE Ok(match)
        print($"syntax={match.matched}|{match.start}|{match.end_index}")
    CASE Error(problem)
        print($"syntax-error={problem.code}")
    END SELECT

    unicode = regex.find("🙂+", "x🙂🙂y")
    SELECT unicode
    CASE Ok(match)
        print($"unicode={match.matched}|{match.start}|{match.end_index}")
    CASE Error(problem)
        print($"unicode-error={problem.code}")
    END SELECT

    empty = regex.find("a*", "bbb")
    SELECT empty
    CASE Ok(match)
        print($"empty={match.matched}|{match.start}|{match.end_index}")
    CASE Error(problem)
        print($"empty-error={problem.code}")
    END SELECT

    unsupported = regex.find("(a)\\1", "aa")
    SELECT unsupported
    CASE Error(problem)
        print($"backref={problem.code}")
    CASE Ok(match)
        print("backref=unexpected-success")
    END SELECT

    invalid = regex.find("[abc", "a")
    SELECT invalid
    CASE Error(problem)
        print($"invalid={problem.code}|{problem.byte_offset}")
    CASE Ok(match)
        print("invalid=unexpected-success")
    END SELECT
END ACTION
