# ADR-0005: Keep AST syntax-only and immutable

Status: Accepted

## Context

Syntax trees must preserve source meaning while semantic passes add type, symbol, effect, and protocol information. Mutating AST nodes for every pass couples unrelated capabilities.

## Decision

AST nodes are immutable after parsing and contain syntax plus source spans only. Name resolution, inferred types, effects, and Actor protocol facts live in semantic side tables indexed by typed Node_Id and Symbol_Id.

## Consequences

- Parser recovery cannot depend on semantic state.
- Re-running or incrementally invalidating a semantic pass does not rewrite the AST.
- Public syntax and semantic APIs use stable IDs and explicit lifetimes.
