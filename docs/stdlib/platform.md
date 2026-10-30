# Platform

Import `@std/platform/platform.bas`. These pure functions report the current
Runtime target:

- `os_name()` returns `windows`, `linux`, `macos`, or `unknown`.
- `architecture()` returns `x86_64`, `aarch64`, `x86`, `arm`, or `unknown`.
- `target_triple()` returns the Runtime compiler target triple.
- `path_separator()` returns the target path separator.
- `line_separator()` returns the target text line separator.

These values describe the binary target, not the host of a remote process.
Environment values and process state belong to the Environment service.
