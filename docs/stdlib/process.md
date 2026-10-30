# Process

**Status: implemented on Linux and Windows.** Import
`@std/process/streams.bas` for Buffer and byte-list adapters. Process operations
are asynchronous System Actor messages; they do not wait in BASIC code.

## Spawn and lifecycle

`Process_Spawn(executable, arguments, replace_environment, environment,
working_directory, reply)` completes with `Result<Int, String>`. The returned
integer is an opaque Runtime resource ID. `arguments` remains a separate
`List<String>` passed to the operating system; the Runtime never invokes a
shell. Pass an explicit executable path when lookup behavior matters.

When `replace_environment` is `FALSE`, the child inherits the current
environment and `environment` must be empty. When it is `TRUE`, `environment`
replaces the child's environment and each entry must have the form
`NAME=VALUE`. An empty working directory inherits the parent's current
directory. A non-empty path sets the child's working directory before exec.

`Process_Wait(id, reply)` completes with the exit code. On POSIX, a signal
termination is reported as `128 + signal number`; Windows reports the numeric
process exit code. Repeated Wait requests after completion return the cached
code. `Process_Terminate(id, reply)` requests termination and completes with
`Result<Unit, String>`; use Wait to collect the final status.

## Standard streams

`Process_Write_Stdin(id, bytes, reply)` writes a `List<Int>` and completes with
the number of bytes written. `Process_Close_Stdin(id, reply)` closes the input
stream. `Process_Read_Stdout(id, max_bytes, reply)` and
`Process_Read_Stderr(id, max_bytes, reply)` complete with `List<Int>` chunks.
Reads on each stream are ordered, and each stream permits one outstanding read.
There is no total ordering guarantee between stdout and stderr. The operating
system pipe provides backpressure; bytes are not silently discarded.

Use `streams.bytes_for_stdin(buffer)` to pass an Encoding Buffer to stdin, and
`streams.buffer_from_read_bytes(values)` to wrap a read result as a Buffer.
Continue reading until `Error("closed")` to observe end of stream. A zero-sized
read succeeds with an empty list.

## Limits and errors

- At most 64 Process resources per Runtime, including pending spawns.
- At most 4096 arguments and 1 MiB combined UTF-8 argument/environment bytes.
- Each read and stdin write is capped at 1 MiB.
- Environment replacement, invalid byte values, and invalid limits are checked
  before submission where possible.
- Stable errors include `invalid_argument`, `not_found`,
  `permission_denied`, `resource_exhausted`, `closed`, `spawn_failed`, and
  `io_error`.

Runtime shutdown sends a graceful termination signal to POSIX children, waits
500 ms, then force-kills remaining children. Windows terminates children
immediately because the general process API has no portable graceful signal.
The Runtime waits for blocking workers and reaps children before teardown.
Accepted completions are suppressed after shutdown begins.

## Example

```basic
SEND Process, Process_Spawn("/usr/bin/printf", ["%s", "hello"],
    TRUE, [], "", SELF)
```

The public request and completion types are registered Runtime builtins. The
complete process lifecycle and platform policy are recorded in
[ADR-0040](../architecture/adr/ADR-0040-process-actor-model.md).
