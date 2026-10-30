NATIVE IMPORT "./float.omi"

EXPORT FUNCTION sqrt(value)
    RETURN math_float(0, value, 0.0)
END FUNCTION
EXPORT FUNCTION sin(value)
    RETURN math_float(1, value, 0.0)
END FUNCTION
EXPORT FUNCTION cos(value)
    RETURN math_float(2, value, 0.0)
END FUNCTION
EXPORT FUNCTION tan(value)
    RETURN math_float(3, value, 0.0)
END FUNCTION
EXPORT FUNCTION asin(value)
    RETURN math_float(4, value, 0.0)
END FUNCTION
EXPORT FUNCTION acos(value)
    RETURN math_float(5, value, 0.0)
END FUNCTION
EXPORT FUNCTION atan(value)
    RETURN math_float(6, value, 0.0)
END FUNCTION
EXPORT FUNCTION atan2(y, x)
    RETURN math_float(7, y, x)
END FUNCTION
EXPORT FUNCTION exp(value)
    RETURN math_float(8, value, 0.0)
END FUNCTION
EXPORT FUNCTION log(value)
    RETURN math_float(9, value, 0.0)
END FUNCTION
EXPORT FUNCTION log10(value)
    RETURN math_float(10, value, 0.0)
END FUNCTION
EXPORT FUNCTION floor(value)
    RETURN math_float(11, value, 0.0)
END FUNCTION
EXPORT FUNCTION ceil(value)
    RETURN math_float(12, value, 0.0)
END FUNCTION
EXPORT FUNCTION round(value)
    RETURN math_float(13, value, 0.0)
END FUNCTION
EXPORT FUNCTION trunc(value)
    RETURN math_float(14, value, 0.0)
END FUNCTION
EXPORT FUNCTION is_finite(value)
    RETURN math_is_finite(value)
END FUNCTION
EXPORT FUNCTION is_nan(value)
    RETURN math_is_nan(value)
END FUNCTION
