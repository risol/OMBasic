#include <math.h>
#include <stdbool.h>
#include <stdint.h>

double om_runtime_math_float(int64_t operation, double x, double y)
{
    switch (operation) {
    case 0: return sqrt(x);
    case 1: return sin(x);
    case 2: return cos(x);
    case 3: return tan(x);
    case 4: return asin(x);
    case 5: return acos(x);
    case 6: return atan(x);
    case 7: return atan2(x, y);
    case 8: return exp(x);
    case 9: return log(x);
    case 10: return log10(x);
    case 11: return floor(x);
    case 12: return ceil(x);
    case 13: return round(x);
    case 14: return trunc(x);
    default: return NAN;
    }
}

bool om_runtime_math_is_finite(double value)
{
    return isfinite(value) != 0;
}

bool om_runtime_math_is_nan(double value)
{
    return isnan(value) != 0;
}
