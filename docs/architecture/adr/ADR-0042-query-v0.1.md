# ADR-0042: Synchronous list Query expressions

- Status: Accepted
- Date: 2026-10-31

## Context

Programs need a concise way to filter, order, limit, and project in-memory
lists. The implementation must retain static typing and fit the existing
native compiler and actor model without introducing a scheduler-level
iterator or implicit parallel work.

## Decision

- Query is an eager, synchronous expression over exactly one `List<T>`.
- Clauses appear in the order `WHERE*`, optional `ORDER BY`, optional `TAKE`,
  required `SELECT`.
- Query expressions are pure. Reads of lexical values and the current actor's
  state are allowed; mutation, actor messaging, spawning, I/O, actions, and
  suspension are rejected.
- Unordered filters and projection are lowered to one fused scan. Unordered
  `TAKE` exits the scan early.
- Ordering is stable. `Int` and `String` keys require an explicit direction.
  `ORDER BY ... TAKE K` uses bounded Top-K selection followed by stable ordering.
- Ordered projection is delayed until row selection and ordering finish. Key
  evaluation occurs for each row that passes the filters; key faults precede
  all projection faults.
- Typed HIR is lowered through a temporary QIR plan directly to MIR. QIR keeps
  references to HIR expressions and is not lowered back to AST.
- The actor scheduler does not observe Query. Long queries remain synchronous
  and can delay later mailbox messages in the same actor.

## Consequences

Query's runtime contract is deterministic and testable with ordinary compiler
conformance cases. The compiler can fuse relational operators while keeping
ordinary expression typing and diagnostics in HIR. Query does not provide
automatic fairness; applications needing responsiveness must divide work into
bounded messages themselves. Benchmarks compare fused scans with equivalent
hand-written loops and Top-K with full ordering plus slicing.
