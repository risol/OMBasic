# ADR-0009: Use descriptor-driven native objects and shadow-root GC

Status: Accepted

## Context

Native machine code can keep managed pointers in registers and stack slots,
which a conservative VM heap cannot enumerate reliably. Actors also need
isolated heaps and predictable object ownership.

## Decision

- Represent String, Record, Message, List, and Map as non-moving managed
  objects with a common header pointing to a static type descriptor.
- Keep Actor heaps isolated. The root Action uses the same heap/context APIs
  without an Actor owner.
- Emit a described shadow-root frame for every generated function. Root managed
  parameters and locals for the function lifetime, and pop the frame on each
  return or restore it to the saved base at a fault boundary.
- Treat managed allocation slow paths as safepoints and use descriptor-driven
  tracing for object fields, containers, Actor state, current Messages, and
  queued transit Messages.
- Use non-moving mark-and-sweep in the first implementation; do not require
  stack scanning or moving-pointer updates.

## Consequences

- Generated code explicitly describes all managed references that collection
  must preserve.
- Runtime object layout can be traced and copied without compiler IR.
- Keeping every managed local rooted for the full function is simple and safe,
  though it can retain objects longer than precise liveness-based roots.
