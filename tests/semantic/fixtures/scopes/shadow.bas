FUNCTION shadow(value)
    VAR value = 1
END FUNCTION

FUNCTION immutable_assignment()
    count = 1
    count = 2
END FUNCTION

FUNCTION mutable_assignment()
    VAR count = 1
    count = 2
END FUNCTION
