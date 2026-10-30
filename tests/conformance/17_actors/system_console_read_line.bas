ACTOR Line_Reader
    VAR successful_lines = 0
    RECEIVE Console_Read_Line_Completed(outcome)
        SELECT outcome
        CASE Ok(line)
            print($"line=[{line}]")
            successful_lines = successful_lines + 1
            SEND Console, Console_Read_Line(SELF)
        CASE Error(problem)
            print($"error={problem}")
            STOP
        END SELECT
    END RECEIVE
END ACTOR

ACTION main()
    reader = SPAWN Line_Reader
    SEND Console, Console_Read_Line(reader)
END ACTION
