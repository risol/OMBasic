# Immutable values and transient storage

OMBasic programs work with immutable `List`, `Map`, `String`, and `Buffer`
values. The compiler and Runtime may use mutable storage while a new value is
being built, then publish it once it is complete. The storage address,
capacity, and reuse strategy are not observable from OMBasic.

```text
OMBasic List<T>
      │
      ▼
private transient storage ── seal ──► published immutable List<T>
```

There is no source-level array, builder type, or ownership annotation. A
transient List cannot be returned, passed to an ordinary operation, stored in a
Message, or used as a normal List before it is sealed.

## Internal List builder

MIR has explicit create, reserve, append, and seal operations. The verifier
tracks builder values across CFG block parameters and permits their use only
by builder operations. LLVM lowering calls private Runtime functions for those
operations. The helper declarations live under `src/runtime/` and are absent
from the installed Runtime headers and native `.omi` type system.

The Runtime uses the List descriptor and traces only the initialized elements.
Unused capacity is never treated as managed data. Growth arithmetic is
checked; builder reserve is bounded by an exact capacity limit; ordinary
append uses geometric growth. Reads, reserve, append, and repeated seal reject
an invalid builder state. A failed operation leaves the builder transient and
safe to collect.

## Query

Query keeps its public `List<T> -> List<R>` contract. A fused scan appends
surviving projected values into a transient builder. Ordered Query builds
candidate rows and key lists in transient builders, seals them for stable
ordering or Top-K selection, then builds the final projection in another
builder. `TAKE` bounds the unordered result reserve and stops the scan after
enough rows survive. The source remains an immutable List and Query syntax is
unchanged.

Stable sort and Top-K scratch arrays are private to the Runtime call. They do
not escape into source values. Runtime List concatenation and stable ordering
also publish sealed results.

The standard List facade implements `reverse` and clipped `slice`/`drop` with
exact transient copies. `map`, `filter`, and `take` lower through Query, which
uses the same transient result builder and preserves source order.

Multi-part String interpolation lowers to a single exact-size Runtime join.
The Runtime roots every input String across allocation, copies the UTF-8 byte
sequences, and returns only the completed immutable String.

Runtime also provides a private growable UTF-8 String builder. It appends
Strings or checked Unicode scalar values and seals once to an immutable String.
The byte encoder uses scalar appends for hex output; public String construction
continues to return only sealed values.

The Text facade's `split` appends String segments to a transient List builder
and seals the result. `join` allocates one final String from the List and
separator. `replace` scans non-overlapping UTF-8 matches and allocates its
exact-size result once, without creating an intermediate List.

JSON compaction writes into one bounded output allocation before the owned
String crosses into the managed Runtime. URI formatting uses multi-part
interpolation, and HTTP request text uses one exact-size join after Query
normalizes its header lines. These call sites construct final Strings directly.

`list_set(values, index, replacement)` is a pure immutable update. Its fallback
path copies the List through a transient builder, replaces the indexed value,
seals the new List, and leaves the input storage untouched. A narrow MIR
last-use optimization can instead pass an eligible fresh List to Runtime's
private consume operation. Eligibility requires one total MIR use, a fresh
sealed producer earlier in the same basic block, and a non-block-parameter
source. Use counts include instruction operands, terminators, and CFG edge
arguments.

Runtime tracks List ownership as unique, shared, or currently being consumed.
List, Record, Message, and Map insertion hooks mark Lists shared before
publishing the reference. Runtime cloning starts cloned storage as unique and
marks it shared if the clone graph contains multiple references to the same
List. A consume call mutates a sealed unique List under an atomic ownership
claim; shared or contended storage falls back to the copy path. This metadata
is conservative and does not change source-visible mutability.

`set_range`, `fill_range`, and `replace_range` use the same copy-based rule.
Each allocates one transient List, copies the retained prefix and suffix, fills
the selected region once, then seals the result. `set_range` and `fill_range`
preserve length; `replace_range` can change it. All three check that the
requested source range is valid before building the result.

`encoding.buffer_from_bytes` first validates each byte, then uses a Query
projection to create the normalized `List<Int>` through transient storage.
`encoding.utf8_encode` uses `string_utf8_bytes` to reserve and append UTF-8 bytes
directly to a transient `List<Int>`, then wraps the sealed List in the public
immutable `Buffer` Record. Byte-safe `List<Int>` values use compact one-byte
Runtime storage. UTF-8 and hex codecs use compact byte builders, while concat,
updates, slicing, reversal, cloning, and GC tracing preserve the compact layout
where valid. HTTP serialization reuses the UTF-8 byte List and bulk appends the
body. Encoding keeps its public `Buffer` record representation.

## Safety boundary

Collection immutability is a language property. Construction-time mutation is
safe because a builder is unpublished. MIR rejects builder escape, Runtime
checks state transitions, generated code roots the builder as a managed List,
and GC traces its initialized prefix. Runtime object cloning rejects transient
storage and seals successfully cloned Lists before they can cross a context.
Consume reuse is enabled only by the verifier-checked MIR proof and Runtime's
shared-state fallback; the compiler does not infer uniqueness from reference
counts. A receive handler can use the same consume path for a List in Actor
state when it has exactly one direct field read in the handler. Actor-specific
initialization and replacement setters retain unique ownership only while no
other direct state field aliases that List; duplicate references and unproven
cases are marked shared and use the copy fallback. Advanced interprocedural
ownership analysis remains future work. See
[ADR-0043](adr/ADR-0043-immutable-values-and-transient-storage.md) for the
architectural decision and the [MIR contract](../compiler/mir.md) for verifier
details.
