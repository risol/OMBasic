# ADR-0013: Actor-local mark-and-sweep collection

Status: Accepted

## Context

The Phase 10 VM stored every heap object in one VM-wide allocation list. That
kept messages and Actor state alive, but it prevented reclaiming an Actor's
objects when their last references disappeared and coupled Actor lifetime to
the lifetime of the whole VM.

## Decision

- Give each Actor a stable, separately allocated heap. Root execution keeps a
  VM-owned heap so values returned through the existing public API remain valid
  until `om_vm_destroy`.
- Run iterative mark-and-sweep for the active Actor at bytecode instruction
  safe points after its byte threshold is reached. Mark stacks use temporary
  object links, so tracing does not recurse or allocate memory.
- Trace the Actor's state slots, its worker's operand stack and call-frame
  locals, and the current handler result. The current decoded message remains
  reachable through handler locals after dispatch.
- Deep-copy messages into mailbox-owned transit heaps when enqueued. At
  dispatch, copy the message into the recipient's heap. Both copies preserve
  graph sharing and cycles; transit heaps keep queued messages alive until
  dispatch or mailbox discard.
- Enforce `max_heap_bytes` against the aggregate bytes in the root heap and all
  Actor heaps. Release an Actor's mailbox, state slots, and heap after its
  `Actor_Down` notifications have been copied to watchers.
- Keep the Actor-local GC threshold configurable through `Om_VM_Limits`; zero
  chooses a bounded default. GC only traces the heap of the Actor owning the
  current worker turn, so concurrent collection does not stop other Actors.

## Consequences

- Cycles within a live Actor heap are retained, while unreachable cycles are
  reclaimed without recursion.
- Runtime object graphs do not cross Actor heap boundaries through messages.
  Mailbox transit heaps account against the aggregate heap limit, and message
  sends can fail on allocation limits.
- Values returned by root `om_vm_execute` calls remain usable until the VM is
  destroyed. Root heap collection and explicit public handles remain outside
  this phase.
- The aggregate heap-byte and collection counters use atomics so independent
  Actor heaps can allocate and collect concurrently. Actor-local heap lists
  remain single-worker-owned.
