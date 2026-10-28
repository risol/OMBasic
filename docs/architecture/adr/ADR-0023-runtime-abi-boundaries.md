# ADR-0023: Separate generated Runtime ABI from Runtime implementation

- Status: Accepted
- Date: 2026-10-29

## Context

The installed Runtime header currently mixes generated-code calls, Runtime
management, embedding concerns, and implementation details.

## Decision

Generated code depends only on installed headers under
`include/ombasic/runtime/abi/`. A deliberately small `embedding.h` is the
host-facing API. Runtime implementation headers remain under `src/runtime/`
and are not installed. Generated program descriptors carry an ABI version,
which the Runtime checks before execution.

## Consequences

- The LLVM backend cannot depend on Runtime-private structures.
- Runtime implementation modules can be split without changing the generated
  ABI accidentally.
- SDK header smoke tests and ABI mismatch tests guard this boundary.
