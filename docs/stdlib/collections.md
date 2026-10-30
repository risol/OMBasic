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
- `take(values, count)`, returning at most the first `count` elements
- `drop(values, count)`, skipping the first `count` positions
- `slice(values, start_index, count)`, clipping the requested range to the List
- `map(values, transform)` and `filter(values, predicate)`, preserving order
- Map `map_length(values)`, `map_is_empty(values)`, and
  `contains_key(values, key)`
- Map `keys(values)`, returning String keys in the Map's insertion order
- Map `get_or(values, key, fallback)`, returning the value or the fallback

`take` treats negative counts as zero. `drop` treats negative counts as zero,
so it returns the full List. `slice` returns an empty List for a negative start
or non-positive count and clips ranges that extend beyond the List. Empty
results retain their inferred element type when the call context provides one.
Untyped local empty collections still require an expected type.

The Map helpers are constrained to `Map<String, V>` because the source language
does not yet expose a stable way to declare a generic Map key type.
