# ADR-0036: Console Line Input Contract

- Status: Accepted and implemented
- Date: 2026-10-30

## Decision

1. Line input is an asynchronous `Console` Actor request:
   `Console_Read_Line(reply_actor)` completes with
   `Console_Read_Line_Completed(Result<String, String>)`.
2. The completion removes the LF terminator and, for CRLF input, the
   immediately preceding CR. A bare CR elsewhere is retained. EOF before any
   bytes returns `Error("end_of_input")`; EOF after one or more bytes returns
   those bytes as the final unterminated line. The next read reports EOF.
3. Input is UTF-8 text. Invalid UTF-8 returns `Error("invalid_utf8")`. A line
   is limited to 64 MiB; on overflow, the service drains through the line
   terminator and returns `Error("resource_exhausted")` so the next request
   starts at the next line. Other stdin failures return `Error("io_error")`.
4. Accepted reads are serialized in request-admission order because stdin is a
   single byte stream. Concurrent requests queue within the bounded Runtime
   system-service queue; queue exhaustion rejects the send synchronously and
   does not create a completion.
5. A stopped reply Actor causes its completion to be dropped. Runtime shutdown
   cancels queued reads and suppresses accepted completions. The active input
   wait must be interruptible by an internal shutdown signal before this
   protocol can be implemented; the Runtime must not be left joining a worker
   blocked indefinitely in `fgets` or equivalent.

## Consequences

`Console_Read_Line` is not advertised until the Runtime has a portable,
cancellable stdin wait and tests for console handles and redirected input.
`print` and `Console_Write` retain their existing independent output
semantics.

## Validation

- LF, CRLF, bare CR, empty line, final unterminated line, and EOF.
- Invalid UTF-8, 64 MiB boundary, and over-limit drain behavior.
- Multiple readers, stopped reply Actor, queue exhaustion, and Runtime
  shutdown interruption on Linux and Windows.
