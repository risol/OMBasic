# Console

`@std/console/console_api.bas` provides source helpers for preparing text
payloads for the built-in asynchronous Console Actor:

- `console_api.write_payload(text)` returns the text unchanged.
- `console_api.write_line_payload(text)` appends one LF.

Callers still send `Console_Write(payload, reply_to)` to `Console` and handle
`Console_Write_Completed(outcome)` in their own Actor. `outcome` is a
`Result<Unit, String>`. The helpers do not send messages, wait for completion,
or hide the Actor boundary. The existing `print` Prelude primitive is
unchanged.

`Console_Write` writes the complete UTF-8 payload as supplied and does not add
a line terminator. It flushes the output stream before returning
`Ok(Unit)`. A write failure completes with `Error("io_error")`; queue
admission or allocation failures reject the send synchronously through the
Runtime fault path and do not produce a completion.

The Runtime submits writes to a bounded worker queue. Completion order is not
guaranteed for concurrent requests, including consecutive sends from one
Actor. If an application needs ordered output, it must wait for each
`Console_Write_Completed` before sending the next write. The payload is owned
by the Runtime after acceptance; a stopped reply Actor causes its completion
to be dropped. Runtime shutdown cancels queued writes and suppresses accepted
completions while waiting for active writes to finish.

`print(text)` is a separate synchronous Runtime primitive: it writes `text`
followed by LF, does not flush, and turns output failure into a Runtime Fault.
It has no recoverable error result and does not promise ordering with
`Console_Write` requests.

`Console_Read_Line(reply)` requests one line and completes with
`Console_Read_Line_Completed(Result<String, String>)`. LF and CRLF terminators
are removed; a bare CR elsewhere is retained. EOF before any bytes returns
`Error("end_of_input")`; EOF after bytes returns the final unterminated line,
and the next read reports EOF. Malformed UTF-8 returns `invalid_utf8`. Lines
are limited to 64 MiB; overflow drains to the next terminator and reports
`resource_exhausted`. Other input failures return `io_error`.

Reads are serialized in request-admission order on the bounded system-service
queue. A full queue rejects the send synchronously and creates no completion.
A stopped reply Actor causes the completion to be dropped. Runtime shutdown
cancels queued reads, suppresses accepted completions, and interrupts an
active wait so it can join promptly. Console input semantics and platform
details are specified in [ADR-0036](../architecture/adr/ADR-0036-console-input-protocol.md).
