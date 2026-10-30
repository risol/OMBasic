# ADR-0037: Network Actor Model

- Status: TCP and connected UDP v1 implemented; expanded integration coverage is pending
- Date: 2026-10-30

## Context

Applications need TCP clients and servers without introducing Future, callback,
or event-loop APIs. Network depends on the Byte/Buffer model in ADR-0031, which
now has a source-level Buffer representation and actor-clone conformance
coverage. Cross-node Distribution serialization remains a release prerequisite.

## Decision

1. TCP and connected UDP are Network v1 transports. UDP uses an explicitly
   bound local port and one connected peer; each send and receive preserves a
   datagram boundary. Multicast and per-message destinations are out of scope.
2. The prelude `Network` system Actor owns the socket table. Listen and connect
   requests return opaque positive Int resource IDs; IDs are never native
   handles or pointers. `Tcp_Accept` returns the accepted connection ID. This
   keeps every operation inside the Actor message model without dynamically
   creating user Actors for native resources.
3. `Tcp_Send` carries the `List<Int>` inside an immutable `encoding.Buffer`
   using the `tcp.payload_bytes` Buffer facade and reports `Result<Int, String>` with
   bytes accepted by the local transport. `Tcp_Receive` reports
   `Result<List<Int>, String>`; `tcp.buffer_from_received_bytes` wraps the
   result. Peer EOF is `Error("end_of_stream")`, distinct from an empty read.
   Only one send and one receive can be outstanding per connection; another
   operation of the same direction completes with `resource_exhausted`.
4. `Tcp_Close` is idempotent. A successful close prevents new operations;
   operations already admitted complete with their actual result or a stable
   `closed` error. EOF is distinct from an empty successful read. Peer reset
   maps to `connection_reset`; bind conflicts map to `address_in_use`.
5. v1 limits each message to 64 MiB, active and pending socket resources to
   1024, and listen backlog to 128. The system service uses the bounded Runtime
   blocking-pool queue. Queue saturation is reported as
   `resource_exhausted` at send admission. There is no hidden unbounded socket
   buffer in the BASIC-facing contract.
6. Deadlines are composed by sending `Timer_After` a timeout message to the
   caller's Actor. Network does not add a second timeout or cancellation
   abstraction. A timed-out caller may send an explicit Close or cancel message
   when that connection protocol supports it.
7. Runtime Shutdown closes registered listeners and connections, cancels queued work,
   suppresses accepted completions, and waits for active workers to exit.
8. Network owns application sockets only. It does not expose or reuse
   Distribution's peer discovery, framing, serialization, or transport
   lifecycle. Distribution errors map to stable String error codes at the
   system Actor boundary.
9. Native socket handles and pointers never cross the language ABI. The public
   protocol uses opaque integer IDs and byte lists; the standard-library
   facade accepts and returns Buffer values at application boundaries.
10. UDP resource IDs share the Network resource table. `Udp_Connect` accepts
    remote and local ports, where local port zero asks the OS for an ephemeral
    port. `Udp_Send` sends one complete datagram; `Udp_Receive` returns one
    datagram or `datagram_too_large` if it exceeds the supplied buffer.

## Stable error codes

`invalid_argument`, `invalid_address`, `address_in_use`, `permission_denied`, `not_found`,
`connection_refused`, `connection_reset`, `closed`, `end_of_stream`,
`datagram_too_large`, `resource_exhausted`, `unsupported`, and `io_error`.

Platform text may vary; code is the program-facing value. Name lookup uses
the operating system resolver. TLS, multicast, half-close, and connection
pooling are outside v1.

## Validation status

Windows and Linux Release conformance verifies TCP bind/accept/connect/send/receive
and connected UDP send/receive on loopback. Error mapping,
partial-send stress, EOF, reset, close races, queue saturation, GC pressure,
and shutdown still need dedicated integration tests. Buffer serialization is
covered separately by the Distribution layer's current serializer tests.
