# ADR-0014: Multi-thread Actor scheduler

Status: Accepted

## Context

The Phase 10 scheduler processed Actors on one thread. Phase 12 adds M:N
execution while preserving run-to-completion for each Receive handler and
keeping Actor heaps isolated. A worker must be able to pause a long handler so
other ready Actors can make progress.

## Decision

- Use one VM-wide FIFO ready queue protected by a mutex and condition variable.
  Windows uses native critical sections and condition variables; POSIX builds
  use pthreads through CMake's `Threads` package.
- Start a bounded worker pool after the root action returns. The default is
  four workers, the maximum is 64, and a failed worker creation uses the
  workers that were created; if none start, execute the pool loop on the caller
  thread. `om_vm_execute` joins all workers before returning.
- Keep one execution stack and call-frame set per worker. Protect Actor
  scheduling state with the scheduler mutex. An Actor is marked running before
  dispatch and cannot be queued for another worker until its current quantum
  releases it.
- Run each Actor for at most the configured instruction quantum (default 256).
  Save its stack, frames, and handler result at a bytecode boundary, enqueue it
  at the tail when work remains, and resume that context on an available
  worker.
- Give every mailbox node its own transit heap. A sender deep-copies a message
  into that heap; the recipient worker copies it into the Actor heap when it
  dequeues the message. This prevents workers from mutating another Actor's
  heap.
- Use atomic counters for aggregate heap usage and VM execution counters. GC
  remains Actor-local and only runs while that Actor owns a worker turn.

## Consequences

- Different Actors may execute simultaneously, while one Actor remains
  exclusive to a single worker and each Receive handler resumes at its saved
  instruction position.
- Instruction quanta provide scheduler fairness for long handlers. Actor
  execution order can vary when multiple workers are enabled; set
  `worker_count = 1` for deterministic scheduling tests.
- Mailbox copies add allocation work, but keep heap ownership explicit across
  worker boundaries. Aggregate heap limits include transit heaps.
- ThreadSanitizer and message stress tests cover the concurrent scheduler. A
  Linux container may need a non-PIE TSan executable because of its virtual
  address layout.
