# Generated-code Runtime ABI

The Generated-Code ABI is the C-callable contract between LLVM-emitted native
programs and the OMBasic Runtime archives. It is compiler-independent but
versioned; it is not a general public C API promise. Generated programs include
`ombasic/runtime/abi.h`. Host applications should use
`ombasic/runtime/embedding.h` for lifecycle operations.

## Version and Program descriptor

`OM_RUNTIME_ABI_VERSION` is currently `1`. The generated Program descriptor
stores `abi_version` as its first field. The Runtime checks that field before
reading any other descriptor member. A mismatch reports `OM_STATUS_ABI_MISMATCH`
and stops before interpreting memory with an incompatible layout.

Every generated native program has a Program descriptor, including programs
without Actors. It identifies the program and interface fingerprints, concrete
type and Actor descriptors, `Actor_Down` layout, source spans, entry Action,
and collection threshold. Generated descriptor arrays and function pointers
are immutable static data owned by the loaded program module. Source-span
paths and all referenced descriptors are borrowed. Keep the module loaded and
unchanged until its Runtime context and Actor system have stopped.

The Runtime reads descriptors; it does not read compiler IR or an artifact
file. Type fingerprints describe canonical semantic types, while native field
offsets and sizes describe the concrete target layout. Protocol fingerprints
use canonical Message and Actor identities rather than process-local IDs.

## Execution and fault boundary

Every generated OMBasic function receives a hidden
`Om_Execution_Context *context` before user parameters. The context carries the
Runtime instance, nullable current Actor, current Actor/root heap, shadow-root
chain, fault boundary, and optional source context. It has no instruction
pointer, operand stack, bytecode image, VM frame, opcode, or instruction
quantum.

Ordinary error propagation uses `Result` and `TRY` as normal MIR control flow.
Runtime faults use a separate boundary. `om_runtime_fault` records its fault
code and source span, then transfers control to the nearest boundary. Before
reporting the fault, the Runtime releases roots pushed after that boundary's
saved root base. A caught fault returns process status 70.

The generated object embeds a source-span table indexed by MIR instruction ID.
Each row stores a source path and one-based line and byte column. Runtime
faults can therefore report the original `.bas` location.

## Ownership and linking

The Program descriptor, type and Actor descriptors, handler pointers, entry
function, and source-span storage are borrowed immutable generated data. They
must outlive execution and any Actor work started from that Program.

The linker combines the generated object, target-specific static Runtime
archives, Native metadata libraries, and platform libraries. Arguments are
passed as an argv array. A linked native program does not need the compiler
process or an LLVM installation at execution time.

The installed compiler/runtime pair reports a precise ABI version mismatch
instead of attempting to continue with incompatible descriptors.
