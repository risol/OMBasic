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
