# UUID

Import `@std/uuid/uuid.bas` for pure UUID conversion helpers.

- `uuid.v4_from_random_bytes(buffer)` requires a validated 16-byte Buffer,
  sets the version 4 and RFC variant bits, and returns the lowercase canonical
  8-4-4-4-12 representation as `Ok(String)`.
- `uuid.parse(text)` accepts the canonical hyphenated hexadecimal form in either
  letter case and returns its 16 bytes as `Ok(Buffer)`.
- `uuid.format(buffer)` formats any validated 16-byte UUID Buffer in lowercase.

Malformed text returns `Error(Uuid_Error("invalid_format"))`; invalid buffers
return `invalid_buffer`, and buffers with the wrong length return
`invalid_length`. Obtain random bytes from the `Secure_Random` Actor; UUID
formatting does not create randomness itself.
