# Collections

Import the List facade with `IMPORT "@std/collections/collections.bas"`.

List operations are synchronous, pure in-memory functions. Lists remain
immutable; operations that combine or transform values return a new List.
List indexes are zero-based. Direct List indexing outside the list, with a
negative index, or with a non-Int index raises the language's runtime index
error. Direct Map indexing with a missing key raises the same error; use
`index_of` or `contains_key` when absence is expected.

The current facade provides:

- `length(values)` and `is_empty(values)`
- `contains(values, sought)` using the language's element equality rules
- `index_of(values, sought)`, returning the first zero-based index or `-1`
- `concat(left, right)`, returning the concatenated List
- `reverse(values)`
- Map `map_length(values)`, `map_is_empty(values)`, and
  `contains_key(values, key)`
- Map `keys(values)`, returning String keys in the Map's insertion order
- Map `get_or(values, key, fallback)`, returning the value or the fallback

The current Map helpers are constrained to `Map<String, V>` because the source
language does not yet expose a stable way to declare a generic Map key type.
The underlying language Map still supports its documented key types.

Map helpers beyond length, emptiness, key presence, ordered keys, and `get_or`
are not yet part of this facade. Operations that must create a generic empty List, including `take`,
`drop`, and `slice`, await a language-level way to infer the element type of
that empty result. Generic function values and specialization are supported:
the conformance suite passes a polymorphic function to a generic higher-order
function and specializes it as both `Int -> Int` and `String -> Bool`. The
collection facade still defers `map` and `filter` because it cannot yet
construct a correctly typed empty output List when the input is empty.
