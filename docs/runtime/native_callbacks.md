# Native Callback Ingress

Native callbacks enter the OMBasic Runtime through a compiler-generated C ABI
trampoline. The `.omi` declaration provides the callback signature, so the
compiler does not need libffi closures or a runtime interpreter.

## Callback path

```text
foreign thread
  -> generated C ABI trampoline
  -> om_runtime_native_callback_post
  -> copy supported ABI values
  -> Actor mailbox
  -> normal Actor worker dispatch
```

The trampoline only validates and copies the callback arguments into owned
mailbox storage. The foreign thread never reads Actor state or the Actor heap,
and never runs an OMBasic handler. The normal worker later receives the copied
Message and invokes its handler with a Runtime execution context.

## Compile-time binding checks

The Native metadata binding must resolve to exactly one OMBasic `MESSAGE` in
the loaded source project, and the current Actor must declare a `RECEIVE` for
that Message. Its field count must match the callback signature, and each
field must have a type compatible with the corresponding callback ABI
parameter. Missing or ambiguous Messages, Messages not accepted by the current
Actor, field-count mismatches, and calls outside `RECEIVE` produce `E0403`;
payload type conflicts produce the normal type mismatch diagnostic `E0401`.

## Thread and payload rules

- A callback may arrive on any foreign thread allowed by the Native library.
- Callback code must not access an Actor's mutable state or managed heap.
- Callback code must not call an Actor handler directly.
- Callback code must copy every supported value before returning to the
  foreign caller; it must not retain a pointer into caller-owned stack data.
- The ingress operation must be safe to call concurrently and must report
  failure without running the handler on the foreign thread.
- Callback payload fields currently support integer aliases, `CBool`, `CFloat`,
  and `CDouble`. Pointer, CString, and Native Record payloads are unsupported.

## Lifetime contract

A callback registration lives only for the duration of its Native call. The
Native library must stop and join every callback thread before returning and
must not invoke the callback after return. Immediately after the Native call,
the generated code unregisters the callback and waits for already-running
ingress operations to finish. The Runtime reports the first ingress failure
through the normal fault path. Each ABI signature has 64 generated trampoline
addresses, so at most 64 simultaneous registrations of the same signature are
available.
