# ADR-0040: Process Actor Model

- Status: Accepted and implemented
- Date: 2026-10-30

## Context

Launching external programs is a privileged, blocking system operation. It
must be explicit, avoid shell string concatenation, and integrate with the
Actor-only asynchronous model.

## Decision

1. A global `Process` System Actor owns child-process resources addressed by
   opaque integer IDs. `Process_Spawn` carries the executable, separate
   `List<String>` arguments, an explicit environment policy, environment
   entries, working directory, and reply Actor. No shell is invoked
   implicitly.
2. `replace_environment = FALSE` inherits the parent environment and requires
   an empty environment list. `TRUE` replaces it with `NAME=VALUE` entries.
   An empty working-directory string inherits the parent's current directory.
3. The service exposes `Process_Write_Stdin`, `Process_Close_Stdin`,
   `Process_Read_Stdout`, `Process_Read_Stderr`, `Process_Terminate`, and
   `Process_Wait`, each with a typed completion Message carrying
   `Result<T, String>`. Stream data crosses the ABI as byte lists; the
   `@std/process/process.bas` facade adapts those lists to and from Buffer.
4. A Wait request reports the numeric exit code. POSIX signal exits map to
   `128 + signal number`; Windows reports the process exit code. Repeated Wait
   requests return the cached status. A stream read after EOF returns `closed`.
5. The Runtime caps Process resources at 64 per Runtime, argument count at
   4096, combined argument/environment UTF-8 bytes at 1 MiB, and each stream
   operation at 1 MiB. OS pipe capacity supplies backpressure; the Runtime
   never silently truncates output.
6. Runtime shutdown sends POSIX `SIGTERM`, waits 500 ms, then sends `SIGKILL`
   to remaining children. Windows uses `TerminateProcess` immediately because
   a general graceful process signal is not portable. The Runtime drains the
   blocking pool and reaps children before teardown. Completions are
   suppressed after shutdown begins.
7. The API does not promise a cross-platform PATH lookup algorithm. The
   operating system resolves the explicit executable path.

## Stable error codes

`invalid_argument`, `not_found`, `permission_denied`, `resource_exhausted`,
`closed`, `spawn_failed`, and `io_error`.

## Validation

Linux and Windows conformance exercise real child processes and their streams.
Linux additionally checks that shell metacharacters remain ordinary argv data
and that the 64-resource limit returns `resource_exhausted` for the next
request. Windows argument quoting follows the platform command-line rules.
