# Native Runtime ABI

The Runtime ABI is the stable internal boundary between generated native code
and `libombasic_runtime`. It is C-callable and compiler-independent. ABI
versioning applies to compiler/runtime pairs; it is not a public C API promise.
Generated programs include `ombasic/runtime/abi.h`; embedders include the small
`ombasic/runtime/embedding.h` lifecycle surface. Runtime implementation and
test operations live in private headers under `src/runtime/` and are not
installed.

`OM_RUNTIME_ABI_VERSION` is stored as the first field in each Program
descriptor. The Runtime checks that field before reading any other Program
descriptor member. An incompatible pair reports a version mismatch and stops
before it can interpret descriptor memory using the wrong layout.

## Execution context

Every generated OMBasic function takes a hidden
`Om_Execution_Context *context` before user parameters. The context contains
the Runtime instance, nullable current Actor, current Actor/root heap, shadow
root chain, fault boundary, and optional source context. It contains no
instruction pointer, operand stack, bytecode image, VM frame, opcode, or
instruction quantum.

Pure Functions receive the context because managed allocations can trigger
collection. Semantic effect checking prevents them from using it for forbidden
side effects.

## Managed values and collection

Int is checked i64, Float is double, Bool is i1 in SSA and may be i8 in memory,
Unit/Null have no required payload, and Actor IDs are u64. String, Record,
Message, List, Map, and CPointer use non-moving managed pointers. CPointer is
an opaque wrapper around a foreign address; it traces the owner of borrowed
results and calls its release function when an owned address becomes
unreachable. The common object header
contains a type descriptor pointer, GC-list link, and flags. `Result<T, E>` is
a concrete LLVM aggregate `{i1 tag, T ok_payload, E error_payload}` after
specialization. The tag is true for Ok. Generated root frames trace only the
active Result payload, including when Result values are nested.

Generated code pushes a described shadow-root frame on entry and pops it on
every return path. Managed parameters and locals remain roots for the complete
native function lifetime in the first implementation. Actor state, the
current Message, and Runtime temporaries are also roots. The first collector
is Actor-local, non-moving mark-and-sweep; allocation slow paths are
safepoints. After a collection, the next allocation threshold is at least the
configured minimum and grows with the live heap. This avoids repeatedly
collecting a mailbox heap whose queued transit messages are all still live.
Mailbox queue entries are roots owned by the mailbox heap and are marked
directly from their object headers while the Actor-system lock is held.

## Recoverable runtime faults

Ordinary error propagation uses `Result` and `TRY` as normal MIR control flow.
Runtime faults use a separate boundary: the root entry and any nested handler
can run through `om_runtime_run`. `om_runtime_fault` records its fault code and
source span, then transfers control to the nearest boundary. Before reporting
the fault, the Runtime releases root frames pushed after that boundary's saved
root base. A caught fault returns process status 70.

The generated object embeds a static source-span table indexed by MIR
instruction ID. Each entry stores a source path and one-based line and byte
column. Runtime diagnostics use the form `file.bas:line:column: runtime fault:`.
The non-local boundary is limited to runtime faults; `TRY`, returns, and
`STOP` remain ordinary control flow.

## Descriptors

The compiler emits linked static descriptors for concrete types, Messages,
Actors, and the Program. They are immutable generated data owned by the native
program module; descriptor arrays, source-span paths, entry functions, and
handler/callback functions are borrowed. Keep the module loaded and its data
unchanged until its Runtime context and Actor system have stopped. Type
descriptors carry kind, size/alignment,
fingerprint, structural field/element/key/value metadata, and optional trace,
copy, equality, hash, and serialization callbacks. Trace callbacks visit
managed references and must not allocate or mutate the heap. Actor descriptors carry
state layout/initialization/tracing and a Message-fingerprint-to-handler table.
The Program descriptor carries program/interface fingerprints, descriptors,
source metadata, and the entry Action pointer. Runtime reads descriptors; it
never reads compiler IR or an artifact file.

Message fingerprints use a domain-separated encoding of the canonical module
path, Message name, and ordered field type fingerprints. Type fingerprints
recursively describe structural container and nominal field types without
incorporating process-local type IDs or native object layout. Actor protocol
fingerprints encode the canonical Actor name and the sorted set of accepted
Message fingerprints. The interface fingerprint combines Message schemas and
Actor protocols; the program fingerprint additionally covers the linked type
descriptor set.

## Actor handler boundary

The Runtime callback ABI is:

```c
Om_Status handler(
    Om_Execution_Context *context,
    void *state_payload,
    const void *message_payload);
```

Generated adapters use the active Runtime context to recover the managed state
and Message objects, then call the lowered OMBasic receive function. The worker
claims one Actor, pops one Message, calls its native handler, then commits
status and requeues the Actor if needed. The handler runs to completion on its
native stack. No continuation is saved or moved between workers.

For programs with Actor descriptors, the native entry wrapper starts the
Runtime worker pool before calling the selected entry Action. After the Action
returns, it waits for outstanding messages to finish before destroying the
execution context.

## Linking

The compiler emits object files and a native entry point. The linker combines
the program object, static Runtime, libraries declared by Native metadata, and
platform libraries. Linker arguments are passed as an argv array. A linked
program runs without the compiler process or LLVM installation.
