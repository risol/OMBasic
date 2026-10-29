# File System Runtime Service


File I/O is exposed through the asynchronous `File_System` Actor. It currently
supports UTF-8 text only:

- `File_Read(path, reply_ref)` completes with `Result<String, String>`.
- `File_Write(path, contents, reply_ref)` completes with
  `Result<Unit, String>` and truncates/replaces the target file.
- Paths are OS paths interpreted relative to the process working directory;
  they are not relative to the project Source Root or SDK root.
- The path and text must be valid UTF-8. Embedded NUL in a path is rejected;
  file contents containing invalid UTF-8 produce a read error.
- Completion Messages are delivered asynchronously to the supplied Actor.
  Blocking operations run on the dedicated worker pool.

The Runtime currently maps operational failures to broad String error values;
it does not expose stable OS error codes or distinguish missing files,
permissions, directory targets, and storage errors. There is no binary I/O,
directory traversal, path normalization API, or cancellation operation. These
would require separate API decisions rather than reusing Import path rules.
Reads of invalid UTF-8 and writes to a directory target also complete with the
recoverable `Error(problem)` branch. Portable conformance covers these cases
alongside missing files. Linux-only cases also write to a protected `/proc`
target and `/dev/full` to verify permission and storage failures return the
same error branch without depending on localized OS error text.

The `system-file-system-utf8-roundtrip` Conformance case performs a write then
read through the completion protocol and checks a multi-byte UTF-8 payload. The
runner executes runtime cases in disposable project copies so filesystem tests
do not leave artifacts in the source tree.

The system-file-missing-file-error Conformance case also checks the 
recoverable Error(problem) completion for a missing path. The current 
contract deliberately tests the Result branch rather than matching platform-specific 
error text.
