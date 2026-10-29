FUNCTION invalid_effect()
    print("not pure")
    RETURN 1
END FUNCTION

ACTION main()
    ignored = invalid_effect()
END ACTION
