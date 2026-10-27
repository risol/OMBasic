# ADR-0004: Use arenas and typed IDs for compiler memory

Status: Accepted

## Context

Compiler data has phase and module lifetimes. Retaining pointers into growable arrays makes ownership and relocation fragile.

## Decision

Use checked allocation wrappers, segmented arenas for phase/module-lifetime objects, vectors for growable arrays, and typed IDs for long-lived cross-table references. Reset and destroy operations are explicit. Arena allocations support alignments up to max_align_t.

## Consequences

- Only Foundation allocation modules and runtime heap may call raw allocators.
- Size arithmetic is checked before allocation.
- AST and semantic records use IDs rather than pointers into vectors.
- Arena reset may poison released bytes in debug builds.
