# Runtime interfaces

OMBasic exposes two different C-facing boundaries:

- Generated native programs use the versioned [generated-code ABI](generated_abi.md).
- Host applications use the smaller [embedding API](embedding_api.md).

Runtime implementation headers under `src/runtime/` are private and are not
installed. The [garbage collector](gc.md), [Actor runtime](actor_runtime.md),
and [Native callback lifetime](native_callbacks.md) describe implementation
behavior relied on by generated code and embedders.
