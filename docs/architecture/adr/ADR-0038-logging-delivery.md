# ADR-0038: Logging Delivery Semantics

- Status: Implemented
- Date: 2026-10-30

## Context

The standard library needs structured diagnostic output. Console already
provides asynchronous output through Actor messages, and its completion
reports whether the write succeeded. Logger must not invent hidden waits or a
second asynchronous abstraction.

## Decision

1. `Log_Event` is an immutable record with UTC timestamp in Unix milliseconds,
   level (`trace`, `debug`, `info`, `warn`, `error`), logger name, message, and
   ordered `List<Log_Field>` key/value fields. Values are text; binary values
   require a future explicit encoding.
2. Pure `format_text(event)` produces one deterministic UTF-8 line. It escapes
   backslash, newline, carriage return, tab, and other C0/C1 control
   characters except U+0000, which current NUL-terminated Native text helpers
   cannot accept; field order is preserved. Formatting does not read the
   clock or write to a sink. Callers supply the event timestamp.
3. `Logger` is a system Actor. Callers format events with `format_text` and
   send the resulting text through `Logger_Write(text, reply)`.
   `Logger_Write_Completed(Result<Unit, String>)` means the bytes were written
   and stdout was flushed. Completion is per write; admission alone is not
   delivery confirmation.
4. v1 writes to the same stdout stream used by Console. File sinks, rotation,
   filtering configuration files, and remote sinks are deferred. Logger
   preserves FIFO order for accepted Logger writes and flushes.
5. Logger uses the Runtime's bounded blocking-pool queue. A full queue or a
   message exceeding the 64 MiB system-service limit completes with
   `resource_exhausted`; it does not silently drop or overwrite events. Sink
   errors map to `io_error` and remain visible in the completion message.
6. `Logger_Flush(reply)` completes after all earlier accepted writes have
   completed. Shutdown stops new admissions, drains accepted events, then
   completes close. Runtime Shutdown cancels queued events, suppresses their
   completion messages, and waits for active sink work to end.
7. Logger does not claim atomic ordering with direct `print` calls or direct
   Console requests. Applications requiring ordering send all output through
   the same Logger and wait for the corresponding completion or flush.

## Stable error codes

`resource_exhausted`, `io_error`, `closed`, and `invalid_event`.

## Validation status

Linux and Windows conformance verifies FIFO write and flush ordering. The
Runtime's shared queue and 64 MiB message limit exercise the
`resource_exhausted` path. Sink-failure injection, dead-reply stress, and
full-queue saturation remain additional coverage opportunities.
