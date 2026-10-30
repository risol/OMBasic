IMPORT "@std/number/number.bas"

MESSAGE Parse_Boolean()

ACTOR Boolean_Parser
    RECEIVE Parse_Boolean()
        parsed = number.parse_bool("true")
        SELECT parsed
        CASE Ok(value)
            print($"value={value}")
        CASE Error(problem)
            print($"error={problem.code}")
        END SELECT
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    parser = SPAWN Boolean_Parser
    SEND parser, Parse_Boolean()
END ACTION
