# JSON

`@std/json/json.bas` is the public facade for immutable JSON values. The
concrete value is an exported `Value(encoded)` Record whose `encoded` field
contains one compact JSON value. JSON APIs validate this field on use, because
the Record constructor itself is public.

`Parse_Error(code, byte_offset, detail)` reports recoverable parse errors.
Offsets count UTF-8 bytes from zero. Codes are stable: `1` syntax, `2` invalid
UTF-8, `3` nesting limit, `4` input-size limit, `5` duplicate object key, and
`6` value-kind or lookup mismatch.

The parser accepts surrounding JSON whitespace but exactly one JSON value.
It preserves exact number tokens, object key order, and string escape spelling
when compacting. Duplicate keys are compared after decoding JSON escapes, so
`"a"` and `"\\u0061"` are duplicates. Nesting is limited to 128 containers
and input to 1 MiB.

The public API provides these operations:

- `parse(source) -> Result<Value, Parse_Error>`
- `stringify(value) -> Result<String, Parse_Error>`
- `kind(value) -> Result<String, Parse_Error>`
- `as_string(value) -> Result<String, Parse_Error>` (decodes JSON escapes)
- `as_boolean(value) -> Result<Bool, Parse_Error>`
- `as_number_text(value) -> Result<String, Parse_Error>` (preserves the token)
- `as_int64(value) -> Result<Int, Parse_Error>` accepts only integer-form
  number tokens within the signed 64-bit range; decimal and exponent notation
  are rejected rather than rounded or truncated.
- `as_float64(value) -> Result<Float, Parse_Error>` accepts any JSON number
  that converts to a finite Float64; overflow and underflow are errors. The
  conversion may round, while `as_number_text` always preserves the source
  number token.
- `is_null(value) -> Result<Bool, Parse_Error>`
- `array(values: List<Value>) -> Result<Value, Parse_Error>`
- `array_length(value) -> Result<Int, Parse_Error>`
- `array_get(value, index) -> Result<Value, Parse_Error>`
- `object(entries: List<Entry>) -> Result<Value, Parse_Error>`
- `object_get(value, key) -> Result<Value, Parse_Error>`
- `object_has(value, key) -> Result<Bool, Parse_Error>`
- `object_keys(value) -> Result<List<String>, Parse_Error>` returns decoded
  keys in insertion order.
- `object_entries(value) -> Result<List<Entry>, Parse_Error>` returns decoded
  keys and their values in insertion order.
- `object_length(value) -> Result<Int, Parse_Error>`
- `null_value()`, `boolean(flag)`, `number(decimal_text)`, and
  `string_value(text)`

`Entry(key, value)` is an immutable Record. Object construction preserves the
List order and rejects duplicate keys. Number construction accepts a JSON
number token as String to avoid silently imposing Float precision or Int range.

`number`, `array`, and `object` return `Result<Value, Parse_Error>` because
their inputs can exceed parser limits or violate JSON constraints. `boolean`,
`null_value`, and `string_value` return a `Value` directly. String construction
uses the Native CString boundary and therefore rejects embedded NUL with the
standard recoverable runtime fault.

The source facade is implemented in ordinary `.bas` modules. Parsing,
validation, compacting, quoting, and span lookup use a private module-scoped
Native interface. The runtime C tests exercise parser edge cases independently;
the conformance case exercises the public facade and SDK source import.
