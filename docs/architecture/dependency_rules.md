# Capability dependency rules

This document is the authoritative dependency policy for the Native AOT
implementation.

## Compiler graph

```text
foundation
  -> source
  -> syntax
  -> module_graph
  -> semantic
  -> HIR
  -> specialization
  -> MIR
  -> target
  -> LLVM backend
  -> compiler pipeline
  -> tooling / CLI

foundation -> linker -> tooling / CLI
```

Native metadata depends on source and syntax contracts. Semantic analysis may
consume metadata contracts, but metadata does not depend on semantic analysis
or Runtime. Runtime consumes only public Runtime ABI and foundation contracts.

The target module owns target ABI descriptions shared by compiler components.
The compiler pipeline coordinates HIR, specialization, MIR, and LLVM emission.
The linker module owns platform linker invocation and native executable
launching; it consumes object paths, library names, and Runtime archive paths
through its request contract. It does not link against Runtime implementation
code. The CLI coordinates these modules and owns user-facing diagnostics.

## Runtime graph

```text
runtime_abi
  -> runtime_core
  -> runtime_gc / runtime_data
  -> runtime_actor
  -> runtime_scheduler
  -> runtime_system_actor / runtime_distribution
```

The low-level distribution protocol and transport may depend on foundation.
Runtime distribution depends on those transport contracts and generated
descriptors.

## Forbidden dependencies

- Foundation has no dependency on source, syntax, semantic, compiler, backend,
  or Runtime domain code.
- Syntax does not depend on semantic analysis or Runtime.
- Semantic analysis does not depend on LLVM, compiler implementation details,
  or Runtime internals.
- HIR and MIR do not include LLVM headers.
- Only `src/backend/llvm/` includes `llvm-c/*` headers.
- Runtime does not depend on source, syntax, module graph, semantic analysis,
  HIR/MIR, LLVM, compiler, or tooling.
- Compiler backends use only public Runtime ABI contracts, never Runtime
  implementation headers.
- A capability may not include another capability's private header.
- Production CMake targets follow the compiler and Runtime DAGs.

## Enforcement

CMake target edges and C includes are checked by
`tools/architecture_check/check.py`. Legacy VM/bytecode symbols are rejected
from source, public headers, and tests. Changes to an allowed edge require an
ADR naming the owner and invariant.
