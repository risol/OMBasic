# URI

Import the URI facade explicitly:

```basic
IMPORT "@std/uri/uri.bas"
```

URI operations are pure functions. `Uri` stores separate presence flags for
the scheme, authority, user information, port, query, and fragment so an
absent query is distinct from an empty query (`?`). `Query_Pair` stores a
decoded name and value.

## API

- `parse(text) -> Result<Uri, Uri_Error>` splits URI references, rejects
  non-ASCII input and NUL, and reports malformed percent triplets with a
  zero-based byte offset. Bracketed hosts, user information, ports, path,
  query, and fragment remain textual values.
- `format(uri) -> String` writes the fields and their presence flags back to
  URI text.
- `is_absolute(text)` checks for a scheme before the first path/query/fragment
  delimiter.
- `default_port(scheme)` returns 80 for common HTTP casing, 443 for common
  HTTPS casing, or -1 for other schemes.
- `percent_encode(text, component)` supports `path`, `query`, `fragment`, and
  `userinfo`, using uppercase hex escapes. Query fields are encoded with
  `query_field` so `&` and `=` remain data.
- `percent_decode(text)` decodes percent triplets and requires the resulting
  bytes to be valid UTF-8. Embedded NUL is preserved.
- `query_encode(pairs)` and `query_decode(text)` use `&` between pairs and the
  first `=` between each name and value. A missing `=` decodes as an empty
  value; an empty whole query decodes to an empty list.
- `remove_dot_segments(path)` applies RFC 3986 section 5.2.4 path cleanup.
- `resolve_reference(base, reference)` accepts parsed `Uri` values and returns
  the resolved `Uri`, applying scheme/authority inheritance, path merge,
  dot-segment removal, and query/fragment presence rules.

## Current parser boundary

Input is limited to 1 MiB. The parser validates ASCII and percent-triplet
syntax, recognizes authority boundaries and bracketed hosts, and preserves
empty query/fragment delimiters. It currently treats scheme, reg-name host,
and port contents as text; full RFC grammar validation for those fields and
IDNA are not implemented. The module does not perform DNS, URL/browser
normalization, or filesystem conversion.

`Uri_Error.code` is stable and currently includes `resource_exhausted`,
`invalid_character`, and `invalid_percent_encoding`. Offsets are zero-based
byte offsets into the ASCII URI input. Percent decoding may also return
`invalid_utf8`.
