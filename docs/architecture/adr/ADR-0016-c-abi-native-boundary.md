# ADR-0016: C ABI Native Boundary

Status: Accepted

## Context

OMBasic needs access to operating-system and third-party libraries while
preserving its Actor isolation guarantees. A Native call may block, return
owned memory, or invoke an OMBasic callback on a foreign OS thread. Those
operations must not gain implicit access to Actor heaps or execute Actor
handlers from an arbitrary thread.

## Decision

- Define Native functions and Native Records in an independent `.omi` format.
  The metadata records the C ABI signature, symbol, effect, blocking status,
  pointer ownership, and callback-to-Message binding.
- Support the C ABI as the only direct ABI. C++ libraries must expose
  `extern "C"` wrappers that catch exceptions before returning across the
  boundary.
- Use libffi to prepare and invoke typed C calls from validated `.omi`
  signatures instead of casting arbitrary symbols to guessed function-pointer
  types.
- Keep Native pointers and handles outside ordinary OMBasic values and
  Messages. Copy bounded CString data explicitly and require Owned pointers to
  name their release function.
- Route foreign-thread callbacks through a typed trampoline. The trampoline
  copies ABI fields into primitive VM values and calls
  `om_vm_native_callback_post`; it must not access Actor state or invoke an
  Actor handler directly.
- `om_vm_native_callback_post` creates a Message in a transit heap and submits
  it through the scheduler-locked mailbox path. The scheduler later runs its
  handler on an Actor worker. Ingress is accepted only during active VM
  execution, and pointer/object VM values are rejected.
- Parsed `.omi` modules supplied to resolver configuration expose supported
  Native signatures and effects to semantic analysis and bytecode emission.
  Module binding validates external functions and keeps call bindings alive for
  the VM lifetime.
- `Om_Omi_Project` loads only the canonical `.omi` paths supplied by project
  configuration, relative to its source root. It does not scan directories or
  infer metadata dependencies.
- Actor calls marked `BLOCKING` run in the bounded blocking pool. The Actor
  saves its stack and frames while waiting; completion records the Native result
  and schedules the Actor worker to resume. Calls outside an Actor handler stay
  synchronous.
- Treat callback delivery as scoped to the Native function call. The Native
  function must complete its callback threads before it returns and must not
  retain the callback for later invocation.
- Report ingress resource exhaustion through the VM's asynchronous error
  channel; invalid schemas, field counts, and non-scalar values return an
  `Om_Status` to the trampoline.

## Consequences

- A foreign callback can communicate with an Actor without re-entering the VM
  or violating Actor isolation.
- Message creation copies callback fields into Runtime-owned storage; callback
  data remains independent of the caller's stack after ingress returns.
- Callback payloads are limited to supported scalar fields. The VM validates
  ABI-to-Message compatibility, converts callback arguments, and creates
  temporary libffi closures for a Native call. The Native function must join
  any callback threads before returning.
- Native Record parameters and return values are marshalled by the FFI binding
  API as C structs. Each binding constructs libffi aggregate types from the
  `.omi` fields and verifies their size and alignment against the host C layout.
  Arguments are copied into aligned temporary storage, and a returned record
  is owned by `Om_Native_Call_Result` until it is destroyed. This is not yet
  exposed through BASIC source-level Native imports; nested Records and Record
  callbacks are unsupported. Pointer fields copy addresses only, leaving
  pointee lifetime and ownership to the caller and Native wrapper. Project
  composition must load its configured `.omi` paths and pass the resulting
  modules to resolver configuration.
