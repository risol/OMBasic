# Encoding

**Status: Buffer, Hex, Base64, and UTF-8 conversion are implemented.** Import
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
capped at 64 MiB. `utf8_encode(String)` and `utf8_decode(Buffer)` provide the
explicit bridge. They preserve U+0000 and report malformed UTF-8 as
`Encoding_Error("invalid_utf8", byte_offset)`; both directions enforce the
64 MiB byte limit. The offset identifies the first invalid UTF-8 sequence in
the Buffer. String remains valid UTF-8 text, while Buffer represents arbitrary
bytes.

The private Native ABI uses a length-delimited `CUtf8Slice` for this bridge.
The older `CString` Native ABI remains NUL-terminated and continues to reject
embedded NUL. Hex and Base64 textual inputs therefore retain the Text module's
CString boundary constraints.

The source implementation uses explicit Int normalization before constructing
the Buffer Record so that the compiler preserves the `List<Int>` field type
across module boundaries. See [ADR-0031](../architecture/adr/ADR-0031-byte-buffer-model.md).
