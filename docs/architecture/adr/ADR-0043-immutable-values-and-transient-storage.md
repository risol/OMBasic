# ADR-0043: Immutable Values and Internal Transient Storage

**Status:** Accepted<br>
**Date:** 2026-10-31

## Context

OMBasic exposes immutable `List`, `Map`, `String`, and `Buffer` values. Building
these values one element at a time through repeated immutable concatenation
would allocate and copy unnecessarily. Query also needs result and ordering
storage that can be filled and sorted before the result becomes observable.

The implementation must preserve the existing source type system and Message
semantics. Storage identity, capacity, and reuse are not observable to source
programs.

## Decision

- Collection values remain immutable in the language. There is no public
  `Array<T>`, mutable collection, builder type, or ownership syntax.
- Runtime and compiler code may use private transient storage while constructing
  a value. A transient List is mutable, traces only its initialized prefix, and
  cannot be read as a List or escape MIR builder operations.
- Sealing transitions a transient List to a published immutable List. Runtime
  rejects append, reserve, or a second seal after publication.
- MIR represents builder create, reserve, append, and seal explicitly. Its
  verifier tracks builder values through block parameters and rejects other
  uses, including return, ordinary collection operations, and calls.
- LLVM lowering maps these verified operations to private Runtime entry points.
  It does not infer ownership or choose a mutation strategy.
- List literals, concatenation, stable ordering, and Query results seal their
  storage before publishing the result.
- Query continues to consume `List<T>` and produce `List<R>`. Its scan,
  candidate, and projection storage remains internal.
- Runtime object cloning rejects transient Lists and publishes cloned Lists as
  sealed values.
- MIR may select a private List consume operation only when the source has one
  total use, comes from an eligible fresh sealed producer in the same basic
  block, and is not a block parameter. The verifier independently checks this
  proof; branches, loops, calls, and other aliases keep the copy path.
- Runtime marks List references shared when storing them in Lists, Records,
  Messages, and Maps. Consume claims unique storage atomically and falls back
  to a copy when storage is shared or another consume is active.
- A receive handler may consume a List loaded from Actor state only when MIR
  proves that the state field has one direct read in the handler. Actor-state
  initialization and replacement setters preserve unique ownership only when
  no other direct state field aliases the List; otherwise they mark it shared.
- These rules do not authorize interprocedural ownership inference.

## Consequences

Transient List storage uses the normal List descriptor and GC tracing layout;
the initialized length is the only traced range. Capacity arithmetic is
checked, reserve limits are exact, and ordinary append keeps geometric growth.
The internal builder contract is absent from source parsing, native `.omi`
types, and the installed Runtime API.

The current implementation covers the internal List builder, Query
integration, copy-based immutable List update APIs, a conservative last-use
consume path with Runtime shared-state fallback, compact byte-backed `List<Int>`
storage, a reusable UTF-8 String builder with checked scalar append, Actor-state
replacement reuse under the verified single-read rule, and direct exact-size
Text replacement, JSON compaction, and URI/HTTP String assembly. Advanced
interprocedural ownership analysis remains deferred.
