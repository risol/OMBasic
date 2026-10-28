# ADR-0008: Use a versioned Runtime ABI and static Program descriptor

Status: Accepted

## Context

Generated native code must call Runtime services without exposing compiler
internals or requiring the Runtime to inspect LLVM modules or sidecar artifacts.

## Decision

- Give each generated OMBasic function a hidden
  `Om_Execution_Context *context` parameter. The context provides Runtime,
  current Actor/heap, shadow roots, and the active fault boundary, with no VM
  frame or instruction state.
- Define Runtime entry points and descriptor layouts as a versioned internal
  C ABI shared by generated code and the linked Runtime.
- Emit static descriptors for concrete types, Messages, Actors, and the
  Program into the native object. Descriptors contain layout, structural
  metadata, tracing/copy/serialization hooks as applicable, handler tables,
  source metadata, fingerprints, and the entry Action.
- Make the Runtime consume descriptor pointers and callbacks only; it never
  reads HIR, MIR, LLVM values, or a serialized compiler artifact.
- Link descriptors and the Runtime into the generated executable.

## Consequences

- Compiler and Runtime compatibility is governed by the internal ABI version.
- Runtime services are available to generated code without embedding LLVM.
- Descriptor correctness is part of code generation and is verified by native
  integration tests.
