# Capability dependency rules

This document is the authoritative dependency policy for the reference implementation.

## Allowed direction

Compiler capabilities form a directed acyclic graph:

~~~text
foundation -> source -> syntax -> module_graph -> semantic -> bytecode -> compiler -> cli
~~~

Runtime capabilities depend on stable lower-level runtime contracts:

~~~text
foundation -> runtime/value -> runtime/vm -> runtime/actor -> runtime/scheduler -> runtime/distribution
~~~

The bytecode format is a shared contract: the compiler emitter and VM may both depend on bytecode/format. The VM does not depend on the compiler.

## Forbidden dependencies

- Foundation has no dependency on source, syntax, semantic, compiler, or runtime domain code.
- Syntax does not depend on semantic analysis or runtime.
- Runtime does not depend on compiler, syntax, or module graph.
- Semantic analysis does not depend on CLI or runtime internals.
- Actor and scheduler code do not depend on compiler modules.
- A capability may not include another capability's internal header.

## Public and internal headers

Public headers live under include/ombasic/<capability>/. Private implementation headers live below src/<capability>/internal/. A private header is visible only to its owning capability.

## Enforcing the rules

CMake targets express dependencies. The architecture checker scans C includes and forbidden APIs. New exceptions require an ADR that identifies the owning capability and the invariant being preserved.
