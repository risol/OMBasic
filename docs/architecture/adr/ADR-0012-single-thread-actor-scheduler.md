# ADR-0012: Deterministic single-thread Actor scheduler

Status: Accepted

## Context

Phase 10 executes Actor code on the Phase 9 single-thread VM. A handler must
finish before its Actor processes another message, while messages sent to a
currently running Actor must wait until that handler returns.

## Decision

- Assign each spawned Actor a monotonically increasing, VM-local `Actor_Id`.
  The VM registry owns one state array, FIFO mailbox, and watcher list per
  Actor. `ActorRef` values carry only the registry ID.
- Keep one FIFO ready queue. An Actor is queued at most once, and each
  scheduler turn removes one message and runs its matching Receive handler to
  completion. The Actor is queued at the tail when more messages remain.
- Enqueue `SEND` and `Actor_Down` notifications without running handlers
  inline. Duplicate `WATCH` calls from the same watcher do not add duplicate
  notifications.
- `STOP` clears the Actor's pending mailbox, marks its final down reason, and
  unwinds its active handler. A Receive that propagates an Error stops with
  `FAILED`; watchers receive `Actor_Down` through their normal mailbox.
- Use a shared VM heap for the Phase 10 baseline. Phase 11 replaces this with
  Actor-local heaps; see ADR-0013.
- Keep local scheduling deterministic. `SPAWN ... AT node` reports an
  unsupported-opcode runtime error until the distributed runtime phase.
- Bound Actor count and total queued messages with configurable VM limits.

## Consequences

- One thread and FIFO queues provide run-to-completion without locks or
  coroutines.
- Actor state cannot be reached through an `ActorRef`; messages carry only
  immutable VM values.
- This ADR records the Phase 10 heap model. Actor heap ownership and mailbox
  copying are defined by ADR-0013.
- Scheduler order is reproducible for the same bytecode and message order.
