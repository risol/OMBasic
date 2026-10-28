# ADR-0013: Native callback ingress through generated trampolines

Status: Accepted

## Context

A Native library may invoke a callback from a foreign thread. That thread must
not enter Actor code directly or access Actor-local managed state.

## Decision

- Generate a C ABI trampoline for each supported callback signature and pass
  it to the Native call. Do not create runtime closures or use libffi.
- Validate and copy supported callback arguments into owned ingress storage,
  then enqueue the matching Message through the Runtime's thread-safe mailbox
  path. The foreign thread never executes an OMBasic handler.
- Restrict callback registration to the Native call that receives the
  trampoline. After that call returns, generated code unregisters the
  registration and drains in-flight ingress operations.
- Require the Native library to stop and join every callback-producing thread
  before returning and never invoke the callback after return.
- Surface ingress failures through the normal Runtime fault path.

## Consequences

- Callback payloads cannot borrow caller stack memory beyond the trampoline.
- The Runtime does not need to preserve a callback registration across Native
  calls or process late callbacks after the registration lifetime.
- Native libraries must uphold the documented stop-and-join lifetime contract.
