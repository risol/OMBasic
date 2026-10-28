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
