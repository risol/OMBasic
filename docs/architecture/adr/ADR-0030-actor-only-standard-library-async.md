# ADR-0030: Actor-Only Asynchronous Standard Library APIs

- Status: Accepted
- Date: 2026-10-30

## Context

OMBasic exposes asynchronous work through Actors, Messages, and Mailboxes.
Runtime services may use workers, operating-system asynchronous facilities, or
native callbacks internally, but those mechanisms must not create a second
user-level concurrency model.

## Decision

1. Pure in-memory computation is exposed as `FUNCTION`.
2. Explicit synchronous side effects are exposed as `ACTION` and document
   blocking behavior where relevant.
3. Long-lived state and work that completes asynchronously are exposed as an
   Actor protocol. Requests and their parameters are Messages; results are
   delivered as Completion Messages to an explicit reply Actor.
4. A system service documents request rejection, service shutdown, requester
   shutdown, ordering, concurrency, resource limits, and ownership.
5. Standard-library public APIs do not expose `async`/`await`, Future, Promise,
   coroutine, task, callback completion, hidden waiting, or a user-managed
   event loop.
6. Runtime worker pools and Native callback ingress remain implementation
   details behind the Actor protocol.

## Consequences

Every asynchronous module must have a reviewable Message protocol and failure
contract before implementation. Runtime-internal callback machinery remains
permitted when it only posts work into the existing Actor scheduler. Existing
service contracts such as File System, Console, and Timer are reviewed against
this decision without changing their behavior implicitly.

## Review checklist

- Does every I/O request identify the reply Actor?
- Can a caller handle completion in a normal `RECEIVE` branch?
- Does any ordinary function wait for an external result?
- Are shutdown and resource-exhaustion outcomes observable as protocol results?
- Are queue and payload limits defined?
