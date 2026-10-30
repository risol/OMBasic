# Standard Library Overview

OMBasic standard-library modules are imported explicitly from the SDK root,
for example `IMPORT "@std/json/json.bas"`. Facades re-export only named public
symbols. Import resolution does not fall back to a project source root.

## Effect and execution model

- Pure computation and in-memory transformations use `FUNCTION`.
- Explicit synchronous side effects use `ACTION`.
- Long-lived mutable state and asynchronous work use `ACTOR` and Messages.
- System I/O requests carry an explicit reply Actor; completion is delivered as
  a Message handled by that Actor.
- Runtime worker pools, blocking calls, and scheduling mechanisms are internal
  implementation details. Standard-library APIs do not expose `async`/`await`,
  futures, promises, callbacks, coroutines, or user-managed event loops.

## Error model

Expected operation failures are returned as `Result<T, E>` or represented by
an explicitly documented service Message. Error codes used for branching are
stable and independent of localized operating-system descriptions. Runtime
faults remain distinct from ordinary operation failures. Current File System
completion Strings carry stable codes for compatibility; a versioned typed
`File_Error` protocol remains pending.

## Module inventory

| Module | Import path | Execution model |
|---|---|---|
| Math | `@std/math/math.bas` | Pure functions |
| Text | `@std/text/text.bas` | Pure UTF-8 text operations |
| Path | `@std/path/path.bas` | Pure logical-path operations |
| JSON | `@std/json/json.bas` | Pure parsing, querying, and serialization |
| Collections | `@std/collections/collections.bas` | Pure immutable collection helpers |
| Console | `@std/console/console.bas` | Console Actor protocol |
| File System | `@std/file_system/file_system.bas` | File System Actor protocol |
| Time | `@std/time/time.bas` | Timer and Clock Actors; Duration helpers |
| Random | `@std/random/random.bas` plus `Secure_Random` Actor | Deterministic PRNG and OS-backed secure byte requests |
| Encoding | `@std/encoding.bas` | Pure Buffer, Hex, and Base64 operations |
| Network | `@std/network/tcp.bas`, `@std/network/udp.bas` | TCP and connected UDP System Actor with opaque resource IDs and Buffer facades |
| Logging | `@std/logging/logging.bas` | Pure event formatting and FIFO Logger Actor output |
| Process | `@std/process/streams.bas` | Process System Actor with opaque resource IDs and Buffer adapters |

`Instant` and UTC `Date_Time` helpers are separate modules at
`@std/time/instant.bas` and `@std/time/datetime.bas`. Each module document
records the subset implemented by the current SDK. Random provides both a
reproducible PRNG and a separate OS-backed secure byte Actor. Network provides
TCP and connected UDP v1; broader stress coverage remains pending. Process provides
cross-platform spawn, stream, termination, and wait operations. Encoding has
pure Buffer, Hex, and Base64 support but does not yet provide binary file I/O
or Native UTF-8 conversion.
