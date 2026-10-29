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

There are no implicit Int/Float conversions and no Float helpers in this first
API. All integer operations inherit the checked overflow rules in
OMBasic Language Standard §37. Tests cover facade import, ordinary source
calls, signed values, boundary behavior, and invalid clamp bounds.
