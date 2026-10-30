# Compression

Import `@std/compression/compression.bas` for one-shot `Buffer` compression.
The current API exports:

- `gzip_compress(buffer)` / `gzip_decompress(buffer)` for RFC 1952 gzip
  members;
- `deflate_compress(buffer)` / `deflate_decompress(buffer)` for RFC 1950
  zlib-wrapped DEFLATE streams.

Each function returns `Result<Buffer, Compression_Error>`. Stable error codes
are `invalid_buffer`, `invalid_format`, `checksum_mismatch`,
`resource_exhausted`, and `unsupported`. Gzip CRC-32 and input-size trailers
and the zlib Adler-32 trailer are checked during decompression. Gzip optional
extra, name, comment, and header-checksum fields are accepted; reserved flag
bits are rejected. Zlib preset dictionaries return `unsupported`.

Input and output are limited to 64 MiB per operation. Decompression grows its
output buffer incrementally and stops at the output bound, including for
high-expansion streams. Operations are synchronous pure transformations; they
do not retain input or output after returning.

The implementation uses the pinned miniz 3.1.2 DEFLATE core bundled with the
Runtime, so it adds no system zlib DLL dependency. ZIP and TAR archive APIs,
streaming compression, and caller-selected compression levels are not yet
provided.
