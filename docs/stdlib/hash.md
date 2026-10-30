# Hash and checksum

Import `@std/hash/hash.bas`. Hash operations accept `@std/encoding.bas`
`Buffer` values and perform no I/O.

| Function | Result | Description |
| --- | --- | --- |
| `sha256(buffer)` | `Buffer` | 32-byte SHA-256 digest |
| `sha256_hex(buffer)` | `Result<String, Hash_Error>` | Lowercase hexadecimal SHA-256 digest |
| `crc32(buffer)` | `Result<Int, Hash_Error>` | IEEE CRC-32 checksum as an unsigned value in `Int64` |

SHA-256 uses an in-tree portable implementation and has no OpenSSL runtime
dependency. CRC-32 is an error-detection checksum and is not cryptographic.

`sha256_hex` and `crc32` reject Buffer values containing integers outside
0..255 and values larger than 64 MiB. `sha256` follows the Buffer precondition
and returns an empty Buffer for a malformed manually constructed Buffer value;
callers should create buffers through the Encoding module. The current Native
bridge converts Buffer bytes to hexadecimal text before hashing, so large
inputs use additional temporary memory.

Stable `Hash_Error.code` values are `invalid_buffer` and
`resource_exhausted`.
