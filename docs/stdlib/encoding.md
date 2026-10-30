# Encoding

**Status: Buffer, Hex, and Base64 are implemented.** Import
`@std/encoding.bas`.

`Buffer` is an immutable Record containing a List of Int bytes in 0..255.
`buffer_from_bytes` copies and validates the input and enforces the 64 MiB
limit. Public operations validate Buffer values again because callers can
construct the public Record directly. `buffer_length`, `hex_encode`,
`hex_decode`, `base64_encode`, and `base64_decode` return Results where
validation or decoding can fail.

Hex output uses lowercase ASCII; decoding accepts either case. Base64 uses the
standard alphabet and canonical `=` padding. Decoding rejects malformed
alphabet, padding, lengths, and nonzero unused trailing bits. Error offsets
are zero-based UTF-8 byte positions in the encoded input. Decoded output is
capped at 64 MiB. The module does not provide implicit String-to-Buffer conversion;
String remains UTF-8 text. Native borrowed byte slices and UTF-8 conversion
helpers remain future work.

The current Native-backed Text boundary rejects embedded NUL because its ABI
uses borrowed NUL-terminated strings. Consequently a String containing U+0000
passed to an Encoding decoder raises a recoverable Runtime fault at that
boundary instead of returning `Encoding_Error`. Ordinary malformed ASCII
input returns the documented stable error code and byte offset.

The source implementation uses explicit Int normalization before constructing
the Buffer Record so that the compiler preserves the `List<Int>` field type
across module boundaries. See [ADR-0031](../architecture/adr/ADR-0031-byte-buffer-model.md).
