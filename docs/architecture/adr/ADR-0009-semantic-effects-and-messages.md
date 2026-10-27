# ADR-0009: Analyze effects, Actor protocols, and serialization separately

Status: Accepted

## Context

The type checker establishes that a `SEND` receiver is an `ActorRef` and its
payload is a nominal `Message`. It does not establish that the Actor handles
that Message, that the payload can cross a node boundary, or that a `FUNCTION`
does not call effectful code.

## Decision

- Keep semantic effect, Actor protocol, and serialization checks in separate
  passes after name resolution and type inference.
- Record direct effects on AST nodes and propagate callable effects over a
  project-wide call graph. Treat unknown built-ins as native and effectful.
- Reject `FUNCTION` summaries containing Actor state access, message sends,
  actor creation, native or external effects, or Action calls. Local mutation
  remains allowed because it cannot escape a Function.
- Collect each Actor's accepted Message symbols from its `RECEIVE` handlers;
  accept `Actor_Down` for every Actor. Reject sends when the target protocol is
  unresolved or does not contain the payload's nominal Message type.
- Check serializability recursively through List, Map, Record, and Message
  fields. Accept primitive values and ActorRef; reject unresolved variables,
  callable values, Result, and all unknown/future type kinds by default.
- Keep semantic diagnostics stable as E0501, E0601, and E0701.

## Consequences

- Effect metadata remains available for IDEs even when the source is invalid.
- Actor protocol compatibility and serialization are independently queryable.
- Native pointer and handle source types will be rejected by the serializer
  once the Native interface type model is added in the FFI phase.
