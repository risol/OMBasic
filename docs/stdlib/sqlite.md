# SQLite

Import `@std/sqlite/sqlite.bas` and spawn `sqlite.Database`. The Actor owns one
connection and accepts `Database_Open`, `Database_Execute`, `Database_Query`,
and `Database_Close` messages. Each request includes a reply Actor ID; results
arrive in the matching `Database_*_Completed` message. Close the Actor's
connection explicitly before stopping the owner.

`Database_Execute` accepts one SQL statement without result columns and returns
the changed-row count and last inserted row ID. `Database_Query` accepts one
statement and returns JSON shaped as `{"columns":[...],"rows":[...]}`. SQLite
NULL, INTEGER, FLOAT, and TEXT values become their JSON equivalents. BLOB
values become `{"$blob":"<base64>"}`. Text must contain valid UTF-8. A query
that returns invalid UTF-8 text fails with `invalid_argument`.

The Runtime bundles the official SQLite 3.54.1 amalgamation. Extension loading
is omitted. Limits are a 4 KiB database path, 1 MiB SQL statement, 2048 result
columns, 65536 rows, and 64 MiB encoded query result. SQLite lock waits are
limited to five seconds. The Runtime serializes database handles while each
Native operation runs; callers should keep SQL statements bounded and avoid
long-running queries. Parameter binding, transactions spanning messages,
multiple statements per request, and connection pooling are not part of v1.

Stable error codes are `busy`, `constraint`, `invalid_argument`,
`resource_exhausted`, `not_found`, `io_error`, `cancelled`, and `invalid_sql`.
`native_code` carries the SQLite result code for diagnostics; branch on
`error_code` for portable behavior.
