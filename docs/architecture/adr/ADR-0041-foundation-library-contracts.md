# ADR-0041: Foundation Library Contracts

- Status: Accepted
- Date: 2026-10-30

## Decision

- Stable public error codes use lower_snake_case ASCII identifiers. Human
  diagnostic detail is not a compatibility key.
- Parse offsets are zero-based UTF-8 byte offsets unless an API explicitly
  states that it reports Unicode scalar indices.
- Recoverable input, conversion, and operating-system failures use `Result` or
  an Actor completion payload. Runtime Faults remain for violated invariants
  and unrecoverable Runtime conditions.
- Binary data uses `Buffer`; `String` remains valid UTF-8 text.
- Allocating operations use checked size arithmetic and documented finite
  limits. Limit failures use `resource_exhausted`.
- Every asynchronous system API documents request acceptance, queue
  saturation, requester shutdown, completion delivery, and Runtime shutdown.
- Actor protocol types remain nominal. A program can deliberately erase a
  typed ActorRef with `Actor_Id(actor_ref)` (or create the zero sentinel with
  `Actor_Id()`) and send a known Message through that identity. Erasure skips
  compile-time recipient protocol checking; Runtime delivery still requires
  the recipient to implement the exact message descriptor, and a failed send
  follows the normal Runtime Fault path.

## Consequences

These rules apply to new foundation modules and are the review baseline for
Number, UTF-8 conversion, binary File I/O, URI, Hash, Environment, and network
services. Actor identity erasure is explicit and is intended for services that
must retain a caller's reply identity without constraining the caller to one
nominal actor protocol. Existing protocols retain their current payload types
until a versioned compatibility decision is made.
