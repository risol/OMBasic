# Environment

Environment is a System Actor for process-specific values. Import the Actor
protocol through the language prelude and send one of these requests with a
reply Actor:

- `Environment_Arguments(reply)` returns the process argument snapshot as
  `Result<List<String>, String>`.
- `Environment_Get(name, reply)` returns an environment variable as
  `Result<String, String>`. A missing variable returns `not_found`; an empty
  value is valid.
- `Environment_Current_Directory(reply)` returns the current directory.
- `Environment_Executable_Path(reply)` returns the executable path where the
  platform exposes it.

The process arguments are copied when the Runtime context starts. Other values
are read when their request runs. Text must be valid UTF-8. Requests are bounded
to 4096 arguments and 1 MiB of aggregate argument text; returned values are
bounded to 1 MiB. Stable errors include `not_found`, `invalid_name`,
`invalid_utf8`, `resource_exhausted`, `unsupported`, and `io_error`.

Environment mutation and process-wide directory changes (`setenv`, `unsetenv`,
and `chdir`) are not supported by this v1 protocol.
