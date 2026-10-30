ACTION main()
    VAR value = ""
    VAR index = 0
    WHILE index < 3
        character = "x"
        IF index = 0 THEN
            value = value + "a"
        ELSE
            value = value + character
        END IF
        IF index = 2 THEN
            value = value + "!"
        END IF
        index = index + 1
    END WHILE
    print(value)
END ACTION
