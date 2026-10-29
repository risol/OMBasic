ACTION main()
    VAR total = 0
    FOR index = 1 TO 5
        IF index = 2 THEN
            CONTINUE
        END IF
        total = total + index
        IF index = 4 THEN
            BREAK
        END IF
    END FOR

    WHILE total < 9
        total = total + 1
    END WHILE

    FOR value IN [10, 20]
        total = total + value
    END FOR

    SELECT total
    CASE 39
        print("control-flow-ok")
    CASE ELSE
        print("control-flow-unexpected")
    END SELECT
END ACTION
