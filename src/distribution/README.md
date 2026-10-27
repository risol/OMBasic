# Distribution capability

`protocol.c` encodes and decodes version-3 frames with a fixed 68-byte
big-endian header. The default frame limit is 16 MiB including the header.
Decode borrows payload bytes and can consume one frame from a larger byte
stream. It validates the complete header and payload bound before the caller
allocates the payload buffer.

The header layout is fixed:

| Offset | Field |
| --- | --- |
| 0 | Four bytes `OMBN` |
| 4 | u16 protocol version (3) |
| 6 | u16 frame type |
| 8 | u32 payload byte length |
| 12 | 16-byte sender node ID |
| 28 | u64 sender Actor ID |
| 36 | 16-byte receiver node ID |
| 52 | u64 receiver Actor ID |
| 60 | u64 Message schema fingerprint |

There is no additional TCP length prefix. The payload immediately follows the
header; the receiver reads the fixed header first, validates its declared
length, then reads that payload.

`serializer.c` encodes Message fields by value tag, using fixed-width
big-endian integers, IEEE-754 binary64 floats, UTF-8 strings, bounded
collections, nested Records/Messages, and distributed ActorRefs. It writes no
C object layout. The frame header carries the top-level Message fingerprint;
the receiver supplies the expected fingerprint and field count. The default
nested collection limit is one million values and the default nesting limit is
64; a separate one-million-value budget caps the total decoded value tree.
Decoded field trees own their storage and are released with
`om_distribution_message_destroy`.

The payload value tags follow `Om_Distribution_Value_Kind`: Null 0, Unit 1,
Bool 2, Int64 3, Float64 4, String 5, List 6, Map 7, Record 8, Message 9,
ActorRef 10, Actor_Id 11, and Down_Reason 12. Composite values carry a schema
fingerprint, field/item count, and recursively encoded values. The receiver
checks the expected top-level schema fingerprint and field count before
returning the decoded Message.

`transport.c` provides blocking TCP listener, connect, accept, frame send, and
frame receive operations. TCP preserves the order of sequential sends on one
connection. The API does not retry a failed send; after a partial write or an
invalid/oversized received frame it closes the connection to avoid continuing
on a truncated stream. Concurrent sends on the same connection require caller
serialization. A received frame's payload remains valid until the next receive
or connection close.

`connection_handshake.c` performs a symmetric HELLO/HELLO_ACK exchange above
the transport. Both peers verify protocol version, distinct node IDs, optional
expected-peer identity, and matching program and interface fingerprints. A
failed handshake closes that connection. This checks build compatibility but
does not authenticate the peer.

Node identities use 128 bits from the operating system random source. Message
schema fingerprints are stable FNV-1a 64-bit hashes over length-prefixed
canonical module paths, Message names, and ordered field type IDs; they are
compatibility identifiers, not authentication tags.

The VM can attach a negotiated peer, map remote ActorRefs to local handles,
route `SEND` frames into normal Actor mailboxes, and process remote `WATCH` and
`ACTOR_DOWN` frames. A stopped remote Actor notifies its watchers; loss of a
peer reports `NODE_UNREACHABLE` through the same `Actor_Down` mailbox path.
`SPAWN ... AT node` waits for the peer to allocate the Actor and acknowledge its
remote ID. The peer queues its initializer and runs it before queued messages
the next time its host calls `om_vm_execute`. The receiver thread performs
validation and mailbox ingress only. Failed sends are never retried; reconnect
deduplication and peer authentication remain unsupported.
