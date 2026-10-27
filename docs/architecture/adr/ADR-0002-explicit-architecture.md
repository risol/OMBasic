# ADR-0002: Apply Explicit Architecture boundaries

Status: Accepted

## Context

The compiler, runtime, and native boundary have different invariants and failure modes. A flat source tree or a general-purpose common module would hide ownership and create unstable dependencies.

## Decision

Use explicit capabilities with public headers, private implementation headers, and composition roots. Foundation contains only cross-capability mechanisms. Compiler phases and runtime capabilities are separate CMake targets connected in one-way dependency order.

## Consequences

- The CLI composes compiler and runtime; lower-level modules never call back into CLI.
- CMake target dependencies and an architecture checker enforce boundaries.
- Cross-capability changes update dependency documentation and, when needed, an ADR.
