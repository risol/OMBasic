# ADR-0018: Persistent bytecode schema fingerprints

Status: Accepted

## Context

Phase 15 needs the same stable Record and Message identities after compilation,
bytecode serialization, and reload. OMBC v2 stored schema symbols and field
names but not field types, so a runtime could not derive the wire fingerprint
from a loaded image.

## Decision

- Advance OMBC to version 3, expand the header to carry 64-bit program and
  interface fingerprints, and append a 64-bit fingerprint to every schema
  record. The decoder rejects older versions rather than guessing missing
  metadata.
- Fingerprint a schema using FNV-1a 64-bit over a domain, canonical module path,
  entity name, field count, and ordered 64-bit field type fingerprints. All
  scalar integers in the hash input use big-endian encoding.
- Fingerprint types structurally from the stable type-kind name and child type
  fingerprints. Nominal Record, Message, and Actor protocol types also include
  their canonical module path and entity name. Unresolved variables and
  callable types cannot produce wire fingerprints.
- Give synthesized system Message schemas fixed names, type labels, and a
  reserved `__ombasic__` module path so their fingerprints do not depend on
  compiler symbol numbering.
- Compute a program fingerprint from each canonical project-relative module
  path and source content hash. Compute the interface fingerprint from the
  emitted ordered schema fingerprints. Store both in the OMBC header so a
  loaded image can initialize a Node handshake without source files.
- Treat fingerprints as compatibility identifiers only. They do not
  authenticate bytecode, peers, or wire messages.

## Consequences

- A loaded bytecode image carries the program/interface fingerprints required
  for the Node handshake and each schema fingerprint required for Message
  serialization, without access to source files or the compiler type store.
- OMBC v3 is incompatible with v1 and v2 images; callers must recompile them.
- FNV-1a is not collision-resistant, so fingerprints must never be used as a
  security boundary.
