# Network

**Status: TCP and connected UDP system Actor protocols implemented.**

Import `@std/network/tcp.bas` for `tcp.payload_bytes` and
`tcp.buffer_from_received_bytes`, the Buffer boundary helpers. Send
`Tcp_Send(connection_id, tcp.payload_bytes(buffer), reply)` to the `Network`
system Actor. `Tcp_Listen(address, port, backlog, reply)` and
`Tcp_Connect(address, port, reply)` complete with an opaque integer resource
ID. `Tcp_Accept(listener_id, reply)` returns a connection ID.
`Tcp_Receive(connection_id, maximum_bytes, reply)` completes with a
`List<Int>`; pass it to `tcp.buffer_from_received_bytes` to wrap it as a
Buffer. A zero byte receive is empty data; peer EOF completes with
`Error("end_of_stream")`. `Tcp_Send_Completed` reports bytes accepted by the
local transport, including partial writes.

For datagrams, import `@std/network/udp.bas` for `udp.payload_bytes` and
`udp.buffer_from_received_bytes`. `Udp_Connect(address, remote_port, local_port, reply)` creates a
connected UDP socket bound to `local_port` (zero requests an ephemeral port)
and connected to its peer. `Udp_Send` sends exactly one datagram and reports
its byte count; `Udp_Receive` returns one datagram as `List<Int>`, preserving
empty datagrams as an empty list. A datagram larger than the requested receive
limit completes with `Error("datagram_too_large")`. `Udp_Close` is idempotent.
The protocol supports IPv4 and IPv6 resolver results and connected unicast
peers; it does not expose multicast or arbitrary per-message destinations.

The Runtime keeps native socket values private. It limits active and pending
resources to 1024, each send/receive to 64 MiB, and backlog to 128. There is
one outstanding send and one outstanding receive per connection; excess
requests complete with `resource_exhausted`. Close is idempotent. Runtime
Shutdown closes registered sockets, wakes pending operations, and suppresses
completions after shutdown begins.

Timeouts use Timer Actor messages addressed to the caller. Network does not
provide a hidden wait, callback, Future, event loop, or second timeout model.
Network owns application sockets and does not expose Distribution's transport
or wire protocol. The complete contract and stable errors are in
[ADR-0037](../architecture/adr/ADR-0037-network-actor-model.md).

The conformance suite covers TCP and UDP loopback send/receive on Linux and
Windows. Error mapping, queue saturation, GC pressure, abrupt TCP peer reset,
and Runtime Shutdown still need dedicated integration coverage. See
[ADR-0037](../architecture/adr/ADR-0037-network-actor-model.md) for the full
contract.
