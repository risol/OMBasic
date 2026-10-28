# ADR-0011: System Actors and the blocking worker pool

Status: Accepted

## Context

Console, file-system, and timer services perform I/O or wait for time to pass.
Running those operations inside an Actor handler would occupy an Actor worker
and undermine the scheduler's bounded concurrency.

## Decision

- Keep system services behind the same Actor/message boundary used by ordinary
  Actors.
- Dispatch blocking Console and File_System work to a bounded blocking pool;
  copy paths, contents, and other task inputs into owned storage before queueing.
- Let Timer own a deep copy of its pending payload and enqueue its completion
  when the deadline expires.
- Return service results as ordinary typed completion Messages carrying
  `Result` values.
- Count pending service work as outstanding Runtime work. Wait and shutdown
  must account for queued work, running work, and completions not yet handled.
- Cancel pending timers and queued blocking work during shutdown, then join
  service threads before destroying Runtime state.

## Consequences

- Blocking operations do not suspend an Actor handler or consume an Actor
  worker while the operation waits.
- Service completion follows normal mailbox ordering and fault handling.
- Shutdown semantics are explicit and bounded by work already running in the
  operating system.
