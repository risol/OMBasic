ACTION main()
    IF FALSE AND (1 / 0 = 0) THEN
        print("bad-and")
    ELSE
        print("and-skipped")
    END IF

    IF TRUE OR (1 / 0 = 0) THEN
        print("or-skipped")
    ELSE
        print("bad-or")
    END IF
END ACTION
