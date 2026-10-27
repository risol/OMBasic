# ADR-0010: Portable versioned bytecode format

- Status: Accepted
- Date: 2026-10-28

## Context

The compiler needs an instruction image that is easy to inspect, verify, store,
and load on different operating systems and C ABIs. Dumping C structures would
make padding, pointer size, enum representation, and endianness part of the file
format. The VM must reject corrupt or incompatible images before execution.

## Decision

Bytecode uses the versioned OMBC format documented in
[the Bytecode capability README](../../../src/bytecode/README.md). The current
format is version 3. It has a fixed-width little-endian header and field-wise
encoded sections for constants, functions, instructions, source maps, Actors,
handlers, and Record/Message schemas. No C structure memory is serialized.

Instruction operands are fixed-width indexes or symbol IDs. Jump targets are
absolute instruction indexes; function and source-map ranges are half-open.
Strings are length-prefixed UTF-8. Decoder allocation is bounded, and the
bytecode verifier runs before decode returns an image.

## Consequences

- An incompatible encoding requires a new format version.
- VM loaders must call the decoder and verifier before dispatching instructions.
- Opcode semantics and operand meanings remain part of the versioned
  compatibility contract.
- The format is independent of host struct layout and host endianness.
- The current format has no optimizer-specific representation; lowering stays
  direct from AST and semantic results until an ADR justifies an intermediate
  IR.
