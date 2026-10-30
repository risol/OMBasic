# Network

**Status: DNS, TCP, connected UDP, and TLS client Actor protocols implemented.**

Import `@std/network/address.bas` for pure numeric address values. `parse_ip`
and `format_ip` handle IPv4 and IPv6 literals; `make_socket_address` validates
ports in `0..65535`, and `format_socket_address` brackets IPv6 hosts. IPv4
parsing rejects ambiguous leading-zero octets. IPv6 formatting uses the
platform's canonical lowercase zero compression. Host names remain distinct
from numeric IP values and belong to DNS resolution.

`Ip_Address.ip_bytes` and `Socket_Address.socket_bytes` contain the raw octets
as `List<Int>` values. IPv4 uses four octets and IPv6 uses sixteen.

`Dns_Resolve(host, port, reply)` runs through the `Network` Actor's blocking
pool and completes with `Result<List<String>, String>`. Successful values are
numeric IPv4 or IPv6 literals in the platform resolver's preference order;
the result order is intentionally platform dependent. `port` must be in
`0..65535`. Errors use stable codes `invalid_host`, `invalid_port`,
`not_found`, `temporary_failure`, `resource_exhausted`, and `io_error`.
No user-visible DNS cache is maintained. Import `@std/network/dns.bas` and call
`dns.socket_address_from_ip_literal(value, port)` to convert an IP literal to
one typed `Socket_Address` value.

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

`Actor_Id(actor_ref)` explicitly erases an ActorRef's protocol type when a
service must retain a reply identity without depending on the caller's actor
protocol. `SEND` accepts this erased identity, while ordinary ActorRef sends
remain protocol checked. The Runtime still checks that the recipient handles
the concrete message descriptor; an unsupported message returns `not_found`
from the Runtime send operation and does not invoke a handler. A failed send
from OMBasic code follows the language's runtime-fault path. Use this
conversion only when the recipient protocol is selected dynamically.

For datagrams, import `@std/network/udp.bas` for `udp.payload_bytes` and
`udp.buffer_from_received_bytes`. `Udp_Connect(address, remote_port, local_port, reply)` creates a
connected UDP socket bound to `local_port` (zero requests an ephemeral port)
and connected to its peer. `Udp_Send` sends exactly one datagram and reports
its byte count; `Udp_Receive` returns one datagram as `List<Int>`, preserving
empty datagrams as an empty list. A datagram larger than the requested receive
limit completes with `Error("datagram_too_large")`. `Udp_Close` is idempotent.
The protocol supports IPv4 and IPv6 resolver results and connected unicast
peers; it does not expose multicast or arbitrary per-message destinations.

TLS upgrades an already connected TCP resource with
`Tls_Client_Start(connection_id, server_name, reply)`. The operation verifies
the peer certificate and server name against the platform trust store; TLS 1.2
is the minimum protocol. On Linux the Runtime uses OpenSSL and system CA
bundles; on Windows it uses Schannel and the Windows certificate store.
Successful setup returns the same opaque resource ID. Use `Tls_Send`,
`Tls_Receive`, and `Tls_Close` for that resource. TCP send/receive operations
are rejected after TLS starts. Errors use stable codes such as
`certificate_invalid`, `hostname_mismatch`, `handshake_failed`,
`protocol_error`, `closed`, and `io_error`.

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

The conformance suite covers localhost and numeric DNS resolution as well as
TCP and UDP loopback send/receive on Linux and Windows. A local test-CA TLS
Actor round trip currently runs on Linux. DNS queue saturation, GC pressure,
abrupt TCP peer reset, Windows Schannel round trip, and Runtime Shutdown still
need dedicated integration coverage. See
[ADR-0037](../architecture/adr/ADR-0037-network-actor-model.md) for the full
contract.
