# Logging

**Status: pure formatter and Logger Actor implemented.**

`Log_Event` carries a caller-supplied UTC timestamp, level, logger name,
message, and ordered text fields. `format_text` emits
`timestamp|level|logger|message|key=value...` followed by a newline. It
escapes backslash, line feed, carriage return, tab, and other C0/C1 control
characters other than U+0000 as `\\`, `\n`, `\r`, `\t`, and lowercase
`\uXXXX` escapes, preserving field order. Text operations currently use a
NUL-terminated Native ABI, so U+0000 is not supported by this formatter.
Formatting is pure and does not read the clock or write to a sink. Send the
formatted string to `Logger` with `Logger_Write(text, reply)`; handle
`Logger_Write_Completed(Result<Unit, String>)` to observe delivery. The Logger
uses the Runtime's bounded blocking-pool queue and writes to the same stdout
stream as Console. Accepted Logger requests are processed in FIFO order.
Queue or message-size limits report `resource_exhausted`; sink failures report
`io_error`. Each write flushes stdout before its completion. `Logger_Flush`
and `Logger_Flush_Completed` provide an explicit ordering point after earlier
accepted writes. Runtime shutdown cancels work that has not started and waits
for active sink work to finish. Direct `print` and Console requests are not
ordered with Logger output.

Import `@std/logging/logging.bas` for `Log_Field`, `Log_Event`, and
`format_text`; `Logger`, `Logger_Write`, `Logger_Write_Completed`,
`Logger_Flush`, and `Logger_Flush_Completed` are prelude Actor APIs.

See [ADR-0038](../architecture/adr/ADR-0038-logging-delivery.md) for lifecycle,
errors, and shutdown semantics. The conformance suite verifies FIFO write and
flush ordering; sink-failure injection and stress coverage for a full shared
blocking-pool queue remain future test improvements.
