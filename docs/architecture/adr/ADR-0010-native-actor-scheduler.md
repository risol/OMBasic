# ADR-0010: Native Actor scheduler and run-to-completion handlers

Status: Accepted

## Context

Native AOT removes the interpreter stack and instruction quantum that formerly
defined Actor scheduling. A handler must therefore run as an ordinary native
call while preserving per-Actor isolation and allowing independent Actors to
make progress concurrently.

## Decision

- Store Actor state, mailbox, lifecycle state, and watcher registrations in the
  Native Runtime; generated code supplies static Actor descriptors and handler
  callbacks.
- Schedule an Actor as one unit. A worker claims at most one handler for that
  Actor at a time, invokes it on its native stack, commits its result, then
  requeues the Actor if eligible messages remain.
- Run handlers to completion. Do not save, migrate, or resume native stacks,
  and do not use a bytecode instruction quantum.
- Permit separate Actors to run concurrently through a bounded worker pool.
- Make STOP discard queued messages and make terminal notification to each
  watcher occur once.

## Consequences

- Actor-local state is never accessed by overlapping handlers for the same
  Actor.
- A CPU-bound handler occupies a worker until it returns; yielding and
  preemption are not part of the initial Runtime model.
- Native stack unwinding is limited to contained Runtime fault boundaries.
