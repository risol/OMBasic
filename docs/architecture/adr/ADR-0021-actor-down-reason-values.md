# ADR-0021: Actor down reasons are built-in nominal values

- Status: Accepted
- Date: 2026-10-29

## Context

`NORMAL`, `FAILED`, and `NODE_UNREACHABLE` are currently lexer keywords and
lower directly to integer-like constants. This makes ordinary identifiers
unavailable and leaks Runtime representation into language syntax.

## Decision

`Actor_Down_Reason` is a standard built-in nominal type. Source code names its
values as `Actor_Down_Reason.normal`, `Actor_Down_Reason.failed`, and
`Actor_Down_Reason.node_unreachable`. The three value spellings are not
keywords and are not available as unqualified magic names. `STOP` remains a
keyword. The Runtime may use compact integers internally, but compiler IR and
fingerprints preserve canonical type and member identities.

`node_unreachable` means the system cannot confirm the remote actor's state;
it is not proof that the actor terminated.

## Consequences

- The lexer and parser treat the values as ordinary qualified-name syntax.
- Type inference and IR retain the nominal type identity.
- The backend maps the built-in members to the Runtime ABI values.
- Protocol fingerprints include canonical type/member identities rather than
  depending only on Runtime integer assignments.
