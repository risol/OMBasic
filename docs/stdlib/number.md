# Number

Import `@std/number/number.bas`. Int parsing is locale-independent and accepts
an optional leading sign followed by digits for the selected radix. It does
not trim whitespace, accept separators, or infer a radix prefix. Radices 2
through 36 are supported; output digits above 9 are lowercase. Parse errors
use stable codes `empty`, `invalid_digit`, `invalid_radix`, and `out_of_range`.
`byte_offset` is a zero-based UTF-8 byte offset; radix errors have offset 0.

The implementation exposes `parse_int`, `parse_int_radix`, `format_int`,
`format_int_radix`, `parse_bool`, `format_bool`, `parse_float`, and
`format_float`. Boolean parsing accepts only lowercase `true` and `false`;
other spellings return `invalid_format`. Float parsing accepts decimal
notation with an optional sign, fraction, and exponent. It rejects whitespace,
NaN, Infinity, and overflow. Underflow follows C round-to-nearest behavior.
Formatting uses 17 significant digits in the C locale for finite round-trip
output; non-finite values return `non_finite`.
