# ADR-0019: Analyze effects, Actor protocols, and serialization separately

Status: Accepted

## Context

Type checking establishes that a `SEND` receiver is an `ActorRef` and its
payload is a nominal `Message`. It does not prove that an Actor handles the
Message, that the payload can cross a node boundary, or that a `FUNCTION` is
free of forbidden effects.

## Decision

- Keep effect, Actor protocol, and serialization checks in separate semantic
  passes after name resolution and type inference.
- Record direct effects and propagate callable effects over the project call
  graph. Treat unknown built-ins as Native and effectful.
- Reject Function summaries containing Actor-state access, sends, actor
  creation, Native/external effects, or Action calls; local mutation remains
  allowed.
- Collect each Actor's accepted Message symbols from its `RECEIVE` handlers
  and accept `Actor_Down` for every Actor.
- Check serializability recursively through List, Map, Record, and Message
  fields. Reject unresolved variables, callable values, Result, and unknown
  type kinds by default.

## Consequences

- Effect, protocol, and serialization diagnostics remain independently
  queryable by tooling.
- Adding a new type kind requires an explicit serialization decision before it
  can cross a node boundary.
