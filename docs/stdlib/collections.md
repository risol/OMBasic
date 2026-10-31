# Collections

Import the List facade with `IMPORT "@std/collections/collections.bas"`.

List operations are synchronous, pure in-memory functions. Lists remain
immutable; operations that combine or transform values return a new List.
The compiler and Runtime may build a result in private transient storage and
seal it before returning the List. No builder or mutable collection type is
part of this facade.
List indexes are zero-based. Direct List indexing outside the list, with a
negative index, or with a non-Int index raises the language's runtime index
error. Direct Map indexing with a missing key raises the same error; use
`index_of` or `contains_key` when absence is expected.

The current facade provides:

- `length(values)` and `is_empty(values)`
- `contains(values, sought)` using the language's element equality rules
- `index_of(values, sought)`, returning the first zero-based index or `-1`
- `concat(left, right)`, returning the concatenated List
- `list_set(values, index, replacement)`, returning a List with one element
  replaced while leaving the input List unchanged
- `set_range(values, start_index, replacements)`, replacing an equal-sized
  range in a new List
- `fill_range(values, start_index, count, replacement)`, filling a range in a
  new List with one value
- `replace_range(values, start_index, remove_count, replacements)`, replacing
  a range with a List whose length may differ
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

Range updates require nonnegative indexes and counts, and the selected range
must fit within the source List. `set_range` preserves the source length;
`fill_range` permits a zero count; `replace_range` removes `remove_count`
elements and inserts all values from `replacements`. Each operation returns a
new List and leaves the input unchanged. An invalid range raises the runtime
index error.

The Map helpers are constrained to `Map<String, V>` because the source language
does not yet expose a stable way to declare a generic Map key type.
