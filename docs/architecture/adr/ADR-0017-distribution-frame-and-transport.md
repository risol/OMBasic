# ADR-0017: Distribution frame and ordered TCP transport

Status: Accepted

## Context

Phase 15 needs a bounded wire format and an ordered transport before remote
Actor messages can be routed. The first increment should remain independent of
the VM until node handshakes, Message serialization, mailbox ingress, and
remote watch semantics are ready.

## Decision

- Give each runtime epoch a fresh 128-bit node identity from the OS random
  source. Actor IDs remain 64-bit values scoped to that node.
- Use a version-3 frame with a 68-byte fixed big-endian header containing magic,
  protocol version, frame kind, payload byte count, sender and receiver
  ActorRefs, and a 64-bit Message schema fingerprint.
- Use blocking TCP streams for the initial transport. Sequential frames on one
  connection retain send order. The transport never retries failed sends;
  partial writes and invalid received frames close the connection.
- Limit complete frames to 16 MiB by default and validate payload length before
  allocating receive storage. Decode borrows payload bytes from its input.
- Compute Message fingerprints with length-prefixed canonical module path,
  entity name, field count, and ordered field type IDs using FNV-1a 64-bit.
  Fingerprints detect interface mismatches; they do not authenticate peers.
- Serialize Message fields by explicit value tags and fixed-width big-endian
  encodings. Never persist C object layout. Bound payload bytes, collection
  lengths, and nesting depth; validate UTF-8 strings and reject schema or field
  count mismatches before accepting a decoded Message. Protocol version 2
  introduced distinct Actor_Id and Down_Reason wire tags; version 3 adds remote
  SPAWN and SPAWN_ACK control frames.
- Exchange HELLO and HELLO_ACK symmetrically above the TCP primitives. Check
  protocol version, distinct node IDs, an optional expected peer, and matching
  program and interface fingerprints. The handshake is compatibility checking,
  not peer authentication.
- Keep Actor scheduling, mailbox mutation, and reconnect policy outside the
  transport and handshake modules.

## Consequences

- Protocol framing, bounded Message payload serialization, and localhost TCP
  ordering can be tested independently of Actor execution.
- Callers must serialize concurrent sends on one connection.
- The VM attaches a negotiated peer, maps remote ActorRefs to local handles,
  routes remote `SEND` frames into ordinary mailboxes, and processes remote
  `WATCH` and `ACTOR_DOWN` control frames. Peer loss delivers
  `Actor_Down(..., NODE_UNREACHABLE)` to local watchers.
- Remote `SPAWN ... AT node` waits for an allocation acknowledgement. The peer
  queues the remote Actor initializer and runs it before queued mailbox messages
  the next time its host calls `om_vm_execute`.
- A VM creates its Node identity and exposes a handshake descriptor using the
  loaded bytecode fingerprints. The transport API returns the validated peer
  descriptor. Failed sends are never retried; request deduplication across
  reconnects and peer authentication remain unimplemented.
- The 64-bit schema fingerprint is an interface compatibility check, not a
  security boundary. Peer authentication remains future work.
