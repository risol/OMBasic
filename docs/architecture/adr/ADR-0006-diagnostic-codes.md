# ADR-0006: Stable diagnostic codes and source spans

Status: Accepted

## Context

The development plan requires deterministic, testable diagnostics. Conformance fixtures need a stable way to identify an error and its location before the full compiler exists.

## Decision

Diagnostic codes are stable public identifiers grouped by compiler stage. Source spans use one-based lines and one-based UTF-8 byte columns, with inclusive starts and exclusive ends. The initial lexical codes are E0101 for keyword casing, E0102 for identifier naming violations, E0103 for malformed lexical tokens, and E0104 for numeric literals outside representable range. E0201 is allocated by the development plan to invalid import paths. The parser uses E0202–E0208 for syntax expectations, illegal module-level constructs, missing expressions, invalid `INT64_MIN` use, chained comparisons, non-call expression statements, and parser safety limits respectively. Name resolution uses E0301–E0307 for unresolved names, private imported members, duplicate declarations, case-only collisions, shadowing, import alias collisions, and assignment to immutable bindings respectively.

## Consequences

- Compile-fail fixtures can assert exact codes and primary spans.
- Message wording can improve without changing the code contract.
- Future diagnostic groups must not reuse an existing code for a different error.
