# ADR-0033: Standard Error Model

- Status: Accepted
- Date: 2026-10-30

## Context

Expected input and operating-system failures must be branchable and portable.
Runtime faults, such as invariant violations or out-of-memory failures in
compiler-generated code, have a different purpose from recoverable operation
errors. Existing public system Actor completions use `Result<T, String>`, so
changing their error payload types would break source compatibility.

## Decision

1. Recoverable operation failures are returned in `Result<T, E>` or a
   documented completion Message. They do not terminate the Runtime.
2. Runtime faults remain outside ordinary `Result` values and indicate a
   failed execution invariant or unrecoverable Runtime condition.
3. Stable error codes are locale independent and suitable for program
   branching. Human-readable detail is diagnostic and is not a compatibility
   key.
4. Until a versioned Actor protocol is introduced, existing File System
   completion payloads remain `Result<T, String>` and the String contains a
   stable code such as `not_found`, `permission_denied`, `invalid_utf8`,
   `resource_exhausted`, or `io_error`.
5. New APIs should use a documented Error Record when the language's public
   type model can express it without breaking the existing protocol. Existing
   payloads migrate only through an explicit protocol or library version.
6. Platform error mapping is centralized per service. Unknown platform errors
   map to that service's stable generic code instead of leaking localized OS
   text into control flow.

## Consequences

Callers can branch consistently across supported platforms while current
File System handlers remain source compatible. The legacy `String` payload is
an interim schema, not a precedent for new APIs. A versioned File System
protocol with `File_Error` remains required before the module can be declared
fully stable.

## Review checklist

- Is the failure recoverable, and is it represented as a value?
- Are stable codes documented and independent of OS message text?
- Are missing, invalid, unsupported, and resource-exhaustion cases distinct?
- Does each platform map equivalent failures consistently?
- Are Runtime faults kept separate from ordinary operation failures?
- Does any error type change preserve protocol version compatibility?
