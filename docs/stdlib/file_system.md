# File System Runtime Service


File I/O is exposed through the asynchronous `File_System` Actor. It supports
UTF-8 text and binary Buffer payloads:

- `File_Read(path, reply_ref)` completes with `Result<String, String>`.
- `File_Write(path, contents, reply_ref)` completes with
  `Result<Unit, String>` and truncates/replaces the target file.
- `File_Append(path, contents, reply_ref)` completes with
  `Result<Unit, String>` and appends UTF-8 text without truncating existing
  contents. The file is created when it does not already exist.
- Text reads are limited to 64 MiB. Write and append requests are limited to
  64 MiB of UTF-8 text each; larger requests complete with
  `Error("resource_exhausted")` without opening or truncating the target.
- `File_Exists(path, reply_ref)` completes with `Result<Bool, String>`.
  Missing paths return `Ok(false)`; an existing file or directory returns
  `Ok(true)`.
- `File_Delete(path, reply_ref)` completes with `Result<Unit, String>` and
  removes one file or symbolic link. It does not remove directories.
- `Directory_Create(path, reply_ref)` completes with `Result<Unit, String>`;
  it creates the directory and any missing parent directories, and succeeds
  when the requested directory already exists.
- `Directory_List(path, reply_ref)` completes with `Result<List<String>, String>`
  and returns only immediate child names in ascending UTF-8 byte order. It
  rejects a non-directory path and returns `resource_exhausted` above 10,000
  entries or 64 MiB of combined name bytes.
- `File_Stat(path, reply_ref)` completes with `Result<List<String>, String>`.
  An `Ok` list has exactly three items: kind (`file`, `directory`, or `other`),
  size in bytes as an unsigned decimal string, and last-modified time as
  signed Unix epoch milliseconds. Stat follows symbolic links. The timestamp
  has millisecond units; platforms whose native file metadata has lower
  precision report the available precision.
- Paths are OS paths interpreted relative to the process working directory;
  they are not relative to the project Source Root or SDK root.
- The path and text must be valid UTF-8. Embedded NUL in a path is rejected;
  file contents containing invalid UTF-8 produce a read error.
- Completion Messages are delivered asynchronously to the supplied Actor.
  Blocking operations run on the dedicated worker pool.
- A request is accepted only after it has been submitted to the bounded
  Runtime worker queue. A rejected send produces the normal Runtime send fault
  and no completion. After acceptance, operation failures are returned through
  `Error(code)`. If the reply Actor has stopped, the completion is dropped.
  Runtime shutdown cancels queued file work and suppresses completions for
  accepted work; callers must not wait for File System completions during
  shutdown.

Operational file failures complete with stable String code values:
`not_found`, `permission_denied`, `already_exists`, `not_directory`,
`is_directory`, `invalid_path`, `invalid_utf8`, `resource_exhausted`,
`unsupported_operation`, and `io_error`. These values do not contain localized
OS error text. The completion payload remains `Result<T, String>` for source
compatibility; a versioned typed `File_Error` completion protocol is still
required before changing that payload type. Binary I/O uses
`File_Read_Bytes`, `File_Write_Bytes`, and `File_Append_Bytes` Actor messages.
Requests and read completions carry `List<Int>` bytes in the inclusive range
0..255; `@std/file_system/file_system.bas` provides Buffer adapters
(`buffer_from_read_bytes` and `bytes_for_write`). Binary reads and writes share
the 64 MiB per-operation limit, preserve every byte including NUL, and report
ordinary filesystem error codes through the completion Result. Binary reads
do not perform UTF-8 validation. There is no directory traversal, path
normalization API, or cancellation operation.
Reads of invalid UTF-8 and writes to a directory target also complete with the
recoverable `Error(problem)` branch. Portable conformance covers these cases
alongside missing files. Linux-only cases also write to a protected `/sys`
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
