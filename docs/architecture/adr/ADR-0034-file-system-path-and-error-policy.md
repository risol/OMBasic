# ADR-0034: File System Path and Error Policy

- Status: Accepted
- Date: 2026-10-30

## Context

The File System Actor currently reads and writes UTF-8 text using paths
resolved by the operating system. Append, stable error codes, directory
operations, and metadata are being added. A typed error payload remains
planned. The API needs explicit path and resource rules before those requests
are added.

## Decision

1. Relative paths are resolved against the process working directory. They are
   not resolved against the source file, project root, or SDK root.
2. Absolute paths are accepted. The service does not impose a root-directory
   sandbox. Operating-system path rules apply, and symbolic links are followed
   by operations that access their targets.
3. `File_Exists` reports whether a path resolves to an existing file or
   directory. A missing path is `Ok(false)`; an access or I/O failure remains
   an error.
4. `File_Delete` removes one file or symbolic link and does not recursively
   remove directories. `Directory_Create` creates the requested directory and
   missing parents. `Directory_List` returns immediate child names only and
   never traverses subdirectories.
5. `Directory_List` ordering is ascending UTF-8 byte order. This is stable
   across supported platforms and does not depend on filesystem enumeration
   order or locale.
6. `File_Stat` follows symbolic links and returns a `Result<List<String>,
   String>`. The successful list contains exactly three entries: kind
   (`file`, `directory`, or `other`), unsigned decimal size in bytes, and
   signed Unix epoch milliseconds for the last modification time. The kind
   and numeric values are strings so this protocol can preserve the existing
   supported completion payload types without adding a new runtime record
   ABI. Timestamp precision is the precision supplied by the platform.
7. File contents remain UTF-8 text in this protocol version. Binary I/O waits
   for the Byte/Buffer API.
8. Existing File System completions keep `Result<T, String>` for source
   compatibility. The String contains one stable error code; localized OS
   detail is not exposed. A typed `File_Error` requires a new, explicitly
   versioned protocol and will not silently replace the current payload.
9. Stable codes are `not_found`, `permission_denied`, `already_exists`,
   `not_directory`, `is_directory`, `invalid_path`, `invalid_utf8`,
   `resource_exhausted`, `unsupported_operation`, and `io_error`. A path that
   is absent in an `Exists` request is the exception and returns `Ok(false)`.
10. A text read is limited to 64 MiB. Each write and append request is limited
   to 64 MiB of UTF-8 text. Directory listing is limited to 10,000 entries and
   64 MiB of combined UTF-8 name bytes. Exceeding any limit returns
   `resource_exhausted`; an oversized write request is rejected before the
   target is opened or truncated.
11. The service performs blocking operating-system work only inside its
    Runtime worker pool. Requests and results remain Actor Messages; no
    standard-library function waits for completion.
12. Request admission uses the Runtime's bounded worker queue. A queue-full or
    allocation failure rejects `SEND` synchronously through the normal Runtime
    fault path and does not produce a completion. Failures after admission use
    the `Error(stable_code)` completion. A stopped reply Actor causes the
    completion to be dropped. During Runtime shutdown, queued work is cancelled
    and all accepted completions are suppressed; shutdown waits for active
    worker calls to finish before releasing the service.

## Consequences

The current process-working-directory behavior is retained. Callers must use
their own path validation when they need confinement. Directory operations
will be deterministic across filesystems. Resource limits and shutdown
behavior must be included in the implementation contract and tests before
those operations are declared stable.

## Validation requirements

- Relative and absolute paths on Linux and Windows.
- Missing paths, file versus directory targets, and symlink behavior.
- Directory creation, non-recursive deletion, and sorted listing.
- Permission failures, invalid UTF-8, resource limits, and unsupported cases.
- Concurrent requests, a terminated reply Actor, queue exhaustion, and Runtime
  shutdown.
- SDK relocation and Actor protocol compatibility.
- Queue-full admission, stopped reply Actors, and Runtime shutdown behavior.
