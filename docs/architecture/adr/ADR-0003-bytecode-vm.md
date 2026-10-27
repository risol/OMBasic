# ADR-0003: Start with a custom bytecode VM

Status: Accepted

## Context

An optimizing native backend would introduce substantial backend complexity before the language, Actor model, GC, and debug semantics are stable.

## Decision

The first backend emits a versioned custom bytecode format and executes it in a VM. LLVM native optimization is deferred. The bytecode format is a stable contract shared by the emitter, verifier, VM, and tooling.

## Consequences

- The verifier is a separate capability and treats artifacts as untrusted input.
- Actor scheduling, GC safepoints, and source maps remain explicit in the runtime.
- A future native backend may be added only after correctness and benchmarks justify it.
