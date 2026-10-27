# ADR-0015: System Actor services

Status: Accepted

## Context

Console and file operations may block in native code, while Actor workers must
remain available to execute other ready Actors. Timers may wait for long
periods and must not occupy either an Actor worker or a blocking-I/O worker.
Service results also need to preserve the language's message-passing model and
must not retain pointers into an Actor-local heap.

## Decision

- Expose `Console`, `File_System`, and `Timer` as reserved ActorRefs loaded by
  `LOAD_SYSTEM_ACTOR`. Advance the bytecode format to OMBC v2 for that opcode.
- Represent requests and replies with built-in Message schemas. Console and
  file requests return `*_Completed(Result<..., String>)` Messages. A Timer
  request carries a delay, Message, and recipient; expiry delivers that Message
  to the recipient.
- Run Console and file operations on a bounded blocking pool. Copy UTF-8 text
  and paths into native task-owned buffers before submission. Workers retain
  stable native data and Actor IDs only, never Actor-heap pointers.
- Use a separate ordered timer service with monotonic deadlines. It does not
  consume a blocking-pool worker while waiting.
- Build every service completion in a transit heap and enqueue it through the
  ordinary mailbox path. Keep the scheduler alive while external tasks remain
  pending and wake it when a completion is enqueued.
- Drain services after normal scheduler completion. After scheduler failure,
  cancel queued blocking requests and pending timers, join active service work,
  then release VM state.
- Interpret file contents as UTF-8 text; writes overwrite the destination.

## Consequences

- Native I/O does not occupy Actor workers, and all service results reach
  Actors through normal `RECEIVE` handlers.
- Blocking-pool bounds provide backpressure; a saturated queue reports an
  error through the existing runtime error path.
- Copying request data and creating transit heaps add allocation and copy
  overhead, while keeping ownership explicit across worker boundaries.
- OMBC v1 images do not contain the new opcode and are rejected by the v2
  loader rather than being interpreted with the wrong instruction set.
- Shutdown waits for active native I/O to return; an OS call that never returns
  can therefore delay VM teardown.
