# Actor Runtime

Generated entry wrappers start the Runtime worker pool when the Program has
Actor descriptors. The selected entry Action runs, and after it returns the
Runtime waits for outstanding messages before destroying the execution
context.

Each worker claims one Actor, removes one Message, invokes its native handler,
then commits the result and requeues the Actor when it still has work. A
handler runs to completion on its native stack. The Runtime does not capture
or move a continuation between workers; `STOP` ends the current Actor after
its active handler returns.

The callback ABI is:

```c
Om_Status handler(
    Om_Execution_Context *context,
    void *state_payload,
    const void *message_payload);
```

Generated adapters use the active Runtime context to recover managed state and
Message objects, then call the lowered OMBasic receive function. Actor state,
Messages, handler pointers, and descriptors are borrowed from the loaded
Program module and remain valid until its Actor system stops.

## Actor termination reasons

The generated ABI uses the private numeric representation
`OM_RUNTIME_ACTOR_DOWN_NORMAL`, `OM_RUNTIME_ACTOR_DOWN_FAILED`, and
`OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE`. OMBasic source uses the nominal
values `Actor_Down_Reason.normal`, `.failed`, and `.node_unreachable`.

`normal` means the Runtime knows the Actor terminated normally. `failed` means
the Runtime knows it terminated after a handler, initializer, or contained
Runtime fault. `node_unreachable` means the local node can no longer confirm a
remote Actor's state; it does not prove that the remote Actor terminated.

Watch notifications enter the watcher's mailbox once and follow the same
run-to-completion ordering as other Messages. The remote-node loss path uses
`Actor_Down_Reason.node_unreachable` for its uncertainty notification.

# Explicit actor identity erasure

`Actor_Id(actor_ref)` converts a typed ActorRef to its opaque runtime identity.
`Actor_Id()` creates the zero sentinel, which is not a live actor reference.
`SEND` accepts either a typed ActorRef or an Actor_Id. Typed sends are checked
against the recipient's nominal protocol during compilation. An erased send
skips that compile-time protocol check, but Runtime delivery still checks the
message descriptor and reports an unsuccessful delivery through the normal
Runtime Fault path. Use the erased form when a service needs to retain a
caller's identity for a typed completion message.
