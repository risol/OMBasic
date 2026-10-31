# Runtime memory management

## Values

Int is checked i64, Float is double, Bool is i1 in SSA and may be i8 in memory,
Unit and Null have no required payload, and Actor IDs are u64. String, Record,
Message, List, Map, and CPointer use non-moving managed pointers. CPointer is
an opaque wrapper around a foreign address; it traces the owner of borrowed
results and calls its release function when an owned address becomes
unreachable.

`Result<T, E>` is a concrete LLVM aggregate `{i1 tag, T ok_payload,
E error_payload}` after specialization. Its tag is true for Ok. Generated root
frames trace only the active Result payload, including when Results are
nested.

## Roots and collection

Generated code pushes a described shadow-root frame on function entry and pops
it on every return path. Managed parameters and locals remain roots for the
complete native function lifetime in the first implementation. Actor state,
the current Message, and Runtime temporaries are roots as well.

The first collector is Actor-local, non-moving mark-and-sweep. Allocation slow
paths are safepoints. After collection, the next allocation threshold is at
least the configured minimum and grows with the live heap. Mailbox queue
entries are roots owned by the mailbox heap and are marked directly from their
object headers while the Actor-system lock is held.

Type trace callbacks visit managed references and must not allocate or mutate
the heap. Descriptor metadata identifies structural fields and active Result
payloads without reading compiler IR.

Transient List builders use the ordinary managed List descriptor. The GC traces
only `length` initialized elements; reserved but unused capacity is not scanned.
Generated MIR values root a builder across allocation safepoints. Sealing changes
its Runtime state but not its managed object address. Object cloning refuses a
transient List and marks a successful List clone as published.

Byte-safe `List<Int>` values may use a compact one-byte backing allocation.
Those bytes are scalar values rather than managed pointers, so tracing skips the
compact buffer. Cloning and builder growth copy the byte payload while preserving
its compact representation.

Published Lists carry atomic ownership metadata for conservative consume-based
updates. Runtime marks a List shared before storing it in a List element,
Record, Message, or Map key/value. Cloning creates new List storage and marks a
clone shared when the source graph references the same List more than once.
Consume claims a unique sealed List atomically; shared or contended Lists use
the normal copy-and-seal path.

Actor-state List replacement uses dedicated initialization and receive-handler
setters. They retain unique ownership only when the stored reference is the
consumed List and no other direct state field aliases it; all other cases mark
the List shared before publication.
