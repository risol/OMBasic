# Standard Library Overview

OMBasic standard-library modules are imported explicitly from the SDK root,
for example `IMPORT "@std/json/json.bas"`. Facades re-export only named public
symbols. Import resolution does not fall back to a project source root.

## Effect and execution model

- Pure computation and in-memory transformations use `FUNCTION`.
- Explicit synchronous side effects use `ACTION`.
- Long-lived mutable state and asynchronous work use `ACTOR` and Messages.
- System I/O requests carry an explicit reply Actor; completion is delivered as
  a Message handled by that Actor.
- Runtime worker pools, blocking calls, and scheduling mechanisms are internal
  implementation details. Standard-library APIs do not expose `async`/`await`,
  futures, promises, callbacks, coroutines, or user-managed event loops.

## Error model

Expected operation failures are returned as `Result<T, E>` or represented by
an explicitly documented service Message. Error codes used for branching are
stable and independent of localized operating-system descriptions. Runtime
faults remain distinct from ordinary operation failures. Current File System
completion Strings carry stable codes for compatibility; a versioned typed
`File_Error` protocol remains pending.

## Module inventory

| Module | Import path | Execution model |
|---|---|---|
| Math | `@std/math/math.bas` | Pure functions |
| Text | `@std/text/text.bas` | Pure UTF-8 text operations |
| Path | `@std/path/path.bas` | Pure logical-path operations |
| JSON | `@std/json/json.bas` | Pure parsing, querying, and serialization |
| Collections | `@std/collections/collections.bas` | Pure immutable collection helpers |
| Console | `@std/console/console.bas` | Console Actor protocol |
| File System | `@std/file_system/file_system.bas` | File System Actor protocol |
| Time | `@std/time/time.bas` | Timer and Clock Actors; Duration helpers |
| Random | `@std/random/random.bas` plus `Secure_Random` Actor | Deterministic PRNG and OS-backed secure byte requests |
| Encoding | `@std/encoding.bas` | Pure Buffer, Hex, Base64, and UTF-8 conversion |
| Number | `@std/number/number.bas` | Pure Int64, Float, Bool parsing and formatting |
| URI | `@std/uri/uri.bas` | Pure URI parsing, formatting, percent/query encoding, and reference resolution |
| Hash | `@std/hash/hash.bas` | Pure SHA-256 and CRC-32 over Buffer values |
| Platform | `@std/platform/platform.bas` | Pure Runtime target facts |
| Environment | System Actor protocol | Process arguments and environment snapshot requests |
| UUID | `@std/uuid/uuid.bas` | Pure UUID parse, format, and v4 construction from Secure_Random bytes |
| CSV | `@std/csv/csv.bas` | Pure bounded CSV parsing and serialization |
| Compression | `@std/compression/compression.bas` | Pure bounded gzip and zlib-wrapped DEFLATE over Buffer |
| Regex | `@std/regex/regex.bas` | Pure bounded Unicode-scalar find with a Thompson NFA |
| SQLite | `@std/sqlite/sqlite.bas` | Serialized Database Actor with bounded SQL and JSON query rows |
| Network | `@std/network/address.bas`, `@std/network/dns.bas`, `@std/network/tcp.bas`, `@std/network/udp.bas` | Typed IP and socket addresses, DNS, TCP and connected UDP System Actor |
| HTTP | `@std/http/http.bas`, `@std/http/client.bas` | HTTP/1.1 wire functions and one-request Network Client Actor |
| Logging | `@std/logging/logging.bas` | Pure event formatting and FIFO Logger Actor output |
| Process | `@std/process/streams.bas` | Process System Actor with opaque resource IDs and Buffer adapters |

`Instant` and UTC `Date_Time` helpers are separate modules at
`@std/time/instant.bas` and `@std/time/datetime.bas`. Each module document
records the subset implemented by the current SDK. Random provides both a
reproducible PRNG and a separate OS-backed secure byte Actor. Network provides
DNS, TCP and connected UDP v1; broader stress coverage remains pending. Process provides
cross-platform spawn, stream, termination, and wait operations. Encoding has
pure Buffer, Hex, Base64, and UTF-8 conversion support. File System binary operations use
List<Int> at the Actor protocol boundary and provide Buffer adapters; the Native
UTF-8 bridge supports embedded NUL through a length-delimited Native ABI. UUID
formats caller-provided random bytes and does not generate randomness itself.

URI parsing and resolution cover the current ASCII and percent syntax subset;
the URI module document records the grammar checks that remain pending.

CSV v1 uses a flattened `Csv_Table` representation and exposes bounded parsing,
field access, and serialization. See [`csv.md`](csv.md) for the grammar and
limits. HTTP provides a bounded Network-backed Client Actor and pure wire
parser/serializer functions. See [`http.md`](http.md) for request lifecycle,
limits, and response messages.

Compression provides one-shot gzip and zlib-wrapped DEFLATE over Buffer values.
See [`compression.md`](compression.md) for checksum validation and output
limits. ZIP and TAR archives are not part of the current implementation.

Regex provides bounded `find` operations with leftmost-shortest
matches indexed by Unicode scalar. See [`regex.md`](regex.md) for supported
syntax, limits, and unsupported constructs.

SQLite is vendored as a pinned amalgamation and exposed through a per-database
Actor. Query completions contain JSON with column names and row values; BLOB
values use a `$blob` base64 object. See [`sqlite.md`](sqlite.md) for lifecycle,
limits, and error handling.

Environment reads process arguments, environment variables, the current
directory, and the executable path through asynchronous System Actor messages.
See [`environment.md`](environment.md) for limits and error codes.

Shared error, offset, and allocation-limit rules are recorded in
[`api_review_checklist.md`](api_review_checklist.md),
[`resource_limits.md`](resource_limits.md), and
[`ADR-0041`](../architecture/adr/ADR-0041-foundation-library-contracts.md).
The HTTP entries in the limit table document the current parser defaults.
