# ADR-0014: Distribution descriptors, fingerprints, and transport

Status: Accepted

## Context

Distributed Actors need a portable wire representation and a way to reject
incompatible programs and Messages. Native object layout and compiler type IDs
are process-local and cannot define a network schema.

## Decision

- Derive program, interface, and Message compatibility fingerprints from
  canonical static descriptors and ordered structural field descriptions;
  never fingerprint C object layout or transient compiler IDs.
- Encode Message schemas from canonical module path, entity name, and ordered
  recursive field type fingerprints. Encode Actor protocols from canonical
  Actor identity and the sorted set of accepted Message fingerprints. Keep the
  interface fingerprint separate from the program fingerprint.
- Treat fingerprints as compatibility checks, not authentication.
- Give each Runtime epoch a fresh 128-bit node identity from the OS random
  source. Actor IDs are 64-bit values scoped to that node.
- Encode values with explicit tags and fixed-width big-endian representations.
  Bound frame size, payload size, collection lengths, and nesting depth before
  allocating untrusted input.
- Use protocol version 3 with a 68-byte fixed big-endian frame header over
  blocking TCP. Validate the 16 MiB default frame bound before allocating the
  payload. Decode borrows its payload bytes from the input buffer.
- Use symmetric HELLO/HELLO_ACK exchange to check protocol version, distinct
  node IDs, an optional expected peer, and matching program/interface
  fingerprints. Keep transport and handshake independent from Runtime
  scheduling and mailbox mutation.
- Serialize Message fields by value, validate UTF-8, and reject schema or
  field-count mismatches. Version 3 includes remote SPAWN and SPAWN_ACK control
  frames.
- Preserve send order for sequential frames on a connection. Do not retry
  failed sends; close connections on partial writes or invalid frames.

## Consequences

- Runtime adapters use generated descriptors to validate remote ActorRefs and
  Messages before mailbox ingress.
- Wire compatibility is independent of platform ABI and compiler layout.
- Concurrent sends on one connection require caller serialization.
- Authentication, reconnect deduplication, and retry policy remain outside the
  initial protocol.
