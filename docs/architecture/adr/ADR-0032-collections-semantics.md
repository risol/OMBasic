# ADR-0032: Collections v1 Semantics

- Status: Accepted
- Date: 2026-10-30

## Context

OMBasic Lists and Maps are immutable language values with insertion-ordered Map
iteration. The standard library should provide pure helpers without adding
mutable collection state or compiler-specific builtins. Source-level generic
Map key declarations and closure capture are not yet available as stable
public contracts.

## Decision

1. Collection helpers are synchronous pure `FUNCTION`s. Operations that
   produce a collection return a new List or Map and leave the input value
   unchanged.
2. List indexes are zero-based. `index_of` returns `-1` if absent.
3. Direct List indexing with a negative, non-Int, or out-of-range index, and
   direct Map indexing with a missing key, produces the language's documented
   runtime index error. Search helpers such as `index_of` and `contains_key`
   provide non-faulting absence checks.
4. `keys` returns keys in the language Map iteration order, and `get_or`
   distinguishes a missing key from a present value by using the existing
   non-faulting `contains_key` check.
5. `reverse` preserves every element and reverses order. Element equality for
   search operations follows the language's existing equality semantics.
6. The first Map facade helpers specialize to `Map<String, V>` because public
   source syntax cannot currently express a stable generic key type. Existing
   Map iteration order remains insertion order.
7. Helpers that must create a generic empty List (`take`, `drop`, and `slice`)
   remain deferred until empty List element type inference can be expressed in
   ordinary source code.
8. Higher-order functions remain deferred until function-value, generic
   specialization, GC, and closure behavior have end-to-end conformance tests.

## Consequences

These semantics require no Runtime changes. Generic empty List construction,
Map helpers for other key types, additional immutable Map transformations, and
higher-order functions remain future work and must preserve the same purity and
ordering rules.

## Validation

Conformance covers empty and non-empty reversal, element order, search
equality, and insertion-ordered Map iteration. Runtime collection and GC tests
remain required before declaring the module stable.
