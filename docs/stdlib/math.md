# Math

`@std/math/math.bas` is the integer Math facade. It re-exports the ordinary
source functions in `integer.bas`; callers must import the facade explicitly.
The current language has no named primitive integer type syntax or overloads,
so arguments and results use normal expression inference and Int64 arithmetic.

| Function | Behavior |
|---|---|
| `add(left, right)` | Checked Int64 addition. Overflow follows the language runtime arithmetic-fault behavior. |
| `abs(value)` | Returns the non-negative magnitude. `abs(INT64_MIN)` overflows and follows the same runtime fault behavior. |
| `min(left, right)` | Returns the lesser Int64 value; ties return `right`. |
| `max(left, right)` | Returns the greater Int64 value; ties return `right`. |
| `clamp(value, lower, upper)` | Bounds `value` inclusively. If `lower > upper`, returns `value` unchanged. |
| `sign(value)` | Returns `-1`, `0`, or `1` according to the Int64 sign. |
| `is_even(value)` | Returns whether the Int64 remainder modulo 2 is zero. |
| `is_odd(value)` | Returns whether the Int64 remainder modulo 2 is nonzero. |

Float functions are re-exported from `float.bas` and call the platform C
mathematics library. They accept and return Float values; there are no implicit
Int/Float conversions. Trigonometric functions use radians. Domain errors
follow IEEE behavior (for example, `sqrt(-1.0)` and `log(-1.0)` produce NaN;
`log(0.0)` produces negative infinity); `is_finite` and `is_nan` inspect these
results. `round` uses the C `round` rule, halfway cases away from zero. Exact
last-bit results for transcendental functions may vary by platform.

Integer operations inherit checked overflow rules in OMBasic Language Standard
§37. Tests cover facade import, ordinary source calls, signed values, boundary
behavior, and invalid clamp bounds.
