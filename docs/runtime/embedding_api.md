# Runtime embedding API

The installed `ombasic/runtime/embedding.h` header is the small lifecycle
surface for host applications. It exposes opaque execution contexts and three
operations:

- `om_runtime_context_create(collection_threshold)` creates a context. A zero
  threshold selects the Runtime default.
- `om_runtime_run(context, entry)` invokes an entry function inside a
  recoverable Runtime fault boundary and returns its process-style result.
- `om_runtime_context_destroy(context)` stops and releases that context.

The embedding surface does not expose Runtime heap layouts, Actor internals,
compiler data structures, or LLVM types. Generated programs should use the
headers in `ombasic/runtime/abi/`; host lifecycle code should use this header.

Execution contexts are thread-confined unless an API explicitly creates a
posting context for an Actor system. Foreign callbacks must enter through the
generated callback ingress ABI; they must not access a managed heap or call an
Actor handler directly. See [Native callback ingress](native_callbacks.md)
for the callback lifetime and drain contract.

The loaded generated module owns all descriptor arrays, source paths, and
function pointers referenced by its Program descriptor. Keep it loaded and
unchanged until its Runtime context and Actor system have stopped.
