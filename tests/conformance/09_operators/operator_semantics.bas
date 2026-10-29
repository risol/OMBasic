ACTION main()
    arithmetic = (8 + 4) * 2 - 7
    comparison = arithmetic >= 17 AND arithmetic <> 0
    IF comparison OR FALSE THEN
        print($"{arithmetic}")
    END IF
END ACTION
