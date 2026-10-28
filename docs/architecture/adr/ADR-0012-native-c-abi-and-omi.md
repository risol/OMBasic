# ADR-0012: Native C ABI calls described by OMI

Status: Accepted

## Context

Native libraries need a stable call boundary, but a runtime-selected dynamic
call engine would reintroduce type erasure and an interpreter-like dispatch
layer. The compiler already knows the target ABI and can lower declared calls
directly.

## Decision

- Retain `.omi` as compile-time metadata for Native symbols, C types, ownership,
  libraries, and callback declarations.
- Resolve Native declarations during semantic analysis and link resolution;
  reject missing, ambiguous, unsupported, or target-incompatible declarations
  before code generation.
- Lower supported fixed-signature calls directly to LLVM functions using
  target-aware C ABI classification. Do not use libffi or runtime symbol
  dispatch for OMBasic calls.
- Keep Native pointers opaque. Make CString conversion and Native pointer
  ownership explicit in the metadata and Runtime APIs.
- Link declared libraries into the generated executable using the selected
  platform toolchain.

## Consequences

- Native calls are ordinary statically typed machine-level calls in the
  generated program.
- C ABI details are target-specific and must be covered by classifier and
  integration tests.
- `.omi` is a compile-time interface description, not a runtime artifact.
