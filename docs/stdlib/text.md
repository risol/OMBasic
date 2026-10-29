# Text

`@std/text/text.bas` exposes the source-level String helpers that can be
implemented safely with current OMBasic syntax:

| Function | Behavior |
|---|---|
| `text.concat(left, right)` | Concatenates two valid UTF-8 strings with `+`. |
| `text.equals(left, right)` | Compares complete String values. |
| `text.is_empty(value)` | Returns whether the value is the empty string. |
| `text.length(value)` | Counts Unicode scalar values, not UTF-8 bytes or grapheme clusters. |
| `text.starts_with(value, prefix)` | Tests an exact Unicode scalar prefix. |
| `text.ends_with(value, suffix)` | Tests an exact Unicode scalar suffix. |
| `text.find(value, needle)` | Returns the first scalar index, or `-1` if absent. An empty needle matches at index 0. |
| `text.find_last(value, needle)` | Returns the last scalar index, or `-1` if absent. An empty needle matches at the end. |
| `text.slice(value, start_index, scalar_count)` | Copies the requested scalar range. Invalid ranges raise a recoverable Native runtime fault. |
| `text.split(value, separator)` | Returns a `List<String>`, preserves empty fields, and splits into scalars when the separator is empty. |
| `text.join(values, separator)` | Joins a `List<String>` with the separator between items; an empty list returns the empty string. |

Splitting on an empty separator returns one String per Unicode scalar; splitting
an empty value on a non-empty separator returns one empty field. Repeated and
trailing separators preserve empty fields. An empty separator is valid for
`join` and concatenates the items directly.

These functions use ordinary `.bas` code and add no compiler special cases.
String equality is value equality. The `+` operator concatenates only two
String values; it does not convert other values. Interpolation formats Int,
Float, Bool, String, and Null using the rules in Language Standard §37.

## Current String and Native boundary

OMBasic String values contain valid UTF-8. Text indexes count Unicode scalar
values; they do not count UTF-8 bytes or grapheme clusters. The API does not
normalize strings, so canonically equivalent but differently encoded scalar
sequences compare and search as distinct values.

The `.omi` `CString` ABI can borrow an OMBasic String for the duration of a
Native call and can copy a Native String result into a managed OMBasic String.
Borrowed arguments reject embedded NUL; returned bytes are checked as UTF-8;
owned Native results are released after copying. This boundary is safe for
individual calls. `NATIVE IMPORT "./text_native.omi"` makes declarations
visible only to the Text source module; it does not add them to the Prelude or
the module's exported interface. The helpers are declared `PURE` and
`NONBLOCKING`, which permits OMBasic `FUNCTION` wrappers to call them without
adding a Native effect.

Every Native-backed operation rejects embedded NUL through the existing
`CString BORROWED` conversion; this produces a recoverable runtime fault. The
slice Native result is owned, copied into a managed OMBasic String, and
released even if copying fails. Negative or out-of-range `find` start indexes
return `-1`; invalid `slice` ranges and allocation failure raise a recoverable
runtime fault. Empty strings are valid; empty prefixes and suffixes match.
