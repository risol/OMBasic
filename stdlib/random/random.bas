EXPORT RECORD Random_Sample(state, value)

FUNCTION normalize_seed(seed_value)
    reduced = seed_value % 2147483647
    IF reduced < 0 THEN
        RETURN reduced + 2147483647
    END IF
    IF reduced = 0 THEN
        RETURN 1
    END IF
    RETURN reduced
END FUNCTION

EXPORT FUNCTION seed(seed_value)
    RETURN normalize_seed(seed_value)
END FUNCTION

' Park-Miller minimal standard PRNG. The sample is an Int in [1, 2147483646].
EXPORT FUNCTION next_sample(generator_state)
    current = normalize_seed(generator_state)
    next_value = (current * 16807) % 2147483647
    RETURN Random_Sample(next_value, next_value)
END FUNCTION
