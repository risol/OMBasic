# ADR-0015: Contained Native Runtime fault boundaries

Status: Accepted

## Context

Native code has no VM instruction loop to catch runtime failures. OMBasic must
still report checked arithmetic and collection faults with source locations,
restore GC roots, and contain a failing Actor handler without corrupting the
Runtime.

## Decision

- Keep ordinary recoverable errors in `Result<T, E>` and `TRY` control flow.
- Represent unrecoverable language Runtime faults with an explicit nested
  boundary around the entry Action, Actor initializers, and Actor handlers.
- Record the fault code and source span, restore the shadow-root chain to the
  boundary's saved base, and return a contained failure to the caller.
- Convert an Actor handler fault into the Actor's normal FAILED terminal state
  so watcher notifications follow the ordinary lifecycle path.
- Limit non-local transfer to Runtime faults; returns, `TRY`, and `STOP` remain
  normal generated control flow.

## Consequences

- A Runtime fault cannot leave generated shadow-root frames linked into a later
  call.
- Fault reporting can include the original OMBasic file, line, and column.
- Foreign exceptions and arbitrary native faults are not translated into
  OMBasic Runtime faults.
