# ADR-0011: Single-thread VM and bounded heap lifetime

Status: Accepted for Phase 9; Actor handling is superseded by ADR-0012

## Context

Phase 9 needs to execute compiler output before the Actor scheduler and garbage
collector exist. The bytecode image is a stable boundary, and the VM must also
be safe when given an image loaded from disk.

## Decision

- Represent values with an explicit tagged union.
- Re-verify the borrowed bytecode image at VM initialization.
- Keep operands on a bounded VM stack and use explicit call frames with local
  arrays; calls return values through the caller's stack.
- Return runtime failures as structured codes with source locations when
  available. Do not use C `longjmp` for language control flow.
- Allocate immutable runtime objects in a VM-owned linked heap and free the
  heap at VM destruction. Add mark-and-sweep in Phase 11.
- Enforce configurable limits for stack values, call frames, heap bytes, and
  executed instructions.
- At the Phase 9 boundary, reject Actor instructions until Phase 10 adds their
  runtime; ADR-0012 records the Phase 10 scheduler decision.

## Consequences

- A VM value that refers to an object is valid only while its owning VM lives.
- The heap can grow until the configured limit because there is no collector
  yet; each VM execution is bounded by that limit.
- Host/native calls remain outside this VM capability. Actor scheduling was
  added in Phase 10 under ADR-0012.
