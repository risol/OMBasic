# ADR-0007: Specialize reachable generic functions before MIR

Status: Accepted

## Context

The initial native backend emits concrete machine signatures. Generic source
functions and collections therefore need finite, deterministic concrete
instances before MIR and descriptor emission.

## Decision

- Key a function instance by its HIR symbol and canonical concrete type
  arguments.
- Discover reachable instances from entry Actions, Actor receive handlers,
  and Actor state initializers using a deterministic worklist.
- Substitute caller instance arguments at direct calls and enqueue newly
  reached instances; deduplicate equivalent canonical keys.
- Reject unresolved or constraint-violating arguments, polymorphic recursion
  with changing arguments, and work exceeding configured safety limits.
- Return a deduplicated set of reachable concrete List, Map, and Result types
  for static descriptor emission.

## Consequences

- MIR functions and Runtime descriptors have concrete types and stable
  signatures.
- Unreachable generic declarations do not produce machine code.
- Polymorphic recursion cannot create an unbounded compilation worklist.
