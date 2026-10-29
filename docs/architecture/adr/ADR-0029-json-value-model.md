# ADR-0029: JSON Value model

- Status: Accepted for OMBasic 0.1
- Date: 2026-10-29

## Context

The 0.1 type system has immutable Records, Lists, Maps, and the built-in
`Result<T, E>`, but no user-declared recursive tagged union. The type store
rejects a recursive type equation such as `T = List<T>`. A direct recursive
JSON tree therefore cannot be expressed by the current source type system.
Adding a new language type feature solely for JSON would expand this plan into
a compiler-language feature project.

## Decision

`JSON.Value` is an immutable Record containing one compact JSON text value.
The parser validates the text and removes insignificant whitespace while
preserving number lexemes, object key order, and string escape spelling. JSON
numbers are not converted to Int or Float, so their decimal precision is not
lost. All operations that consume a `Value` validate its encoded text first,
because the public Record constructor can be called directly.

`JSON.Entry` represents one object key/value pair. `JSON.object` accepts a
List of Entries so callers control key order and duplicate keys can be
rejected consistently. `JSON.array` accepts a List of Values.

The parser accepts exactly one RFC 8259 value, permits surrounding JSON
whitespace, and rejects trailing data, invalid UTF-8, invalid escapes,
unpaired UTF-16 surrogate escapes, duplicate decoded object keys, nesting
deeper than 128 containers, and input larger than 1 MiB. Parse offsets are
zero-based UTF-8 byte offsets. These limits apply to parsing and accessors.

The public API is ordinary `@std/json/json.bas` source. A module-scoped
Native metadata import may provide parsing, quoting, and span lookup helpers;
there is no JSON-specific compiler builtin. Parse failures use a stable
`Parse_Error(code, byte_offset, detail)` Record inside `Result`.

## Consequences

- Values are convenient to store and pass across Modules without defining a
  recursive language type.
- Exact number tokens and object order survive parse/stringify round trips.
- Array and object accessors parse the compact text on each call; the initial
  implementation favors a small stable ABI over a retained native pointer.
- Callers can construct a malformed `Value` Record directly, so public
  operations return `Result` and revalidate instead of trusting the field.
- A future recursive sum type can replace the representation behind the
  facade if OMBasic gains that language feature.

## Validation

Conformance must cover standard and nested JSON, duplicate decoded keys,
number boundaries and preservation, Unicode escapes, byte offsets, depth and
size limits, array/object access, and SDK relocation. Native tests must cover
the parser independently of the Source facade.
