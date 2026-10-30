# ADR-0031: Byte and Buffer Model

- Status: Accepted
- Date: 2026-10-30

## Context

The standard library needs a binary value for Hex/Base64, binary file I/O,
and Network messages. `String` is UTF-8 text and cannot represent arbitrary
bytes. OMBasic currently has immutable Lists and Records, a tracing garbage
collector, Actor message cloning, and Distribution serialization for supported
language values. It does not have a primitive Byte type or a byte-slice Native
ABI.

This decision establishes a safe first representation without exposing a C
pointer or adding a special language type before the compiler and Runtime have
an end-to-end design for one.

## Decision

1. A byte is represented publicly as an `Int` in the inclusive range 0..255.
   Byte-taking APIs validate this range and return a typed error for values
   outside it. The standard library does not reinterpret signed integers or
   characters as bytes.
2. `Buffer` is an immutable Record containing an immutable `List<Int>` of
   bytes. Its constructor and every API accepting a Buffer validate all
   elements and enforce the maximum length of 64 MiB. Since Record constructors
   are public, the `Buffer` invariant is checked at each public boundary; code
   must not assume that a caller used the checked constructor.
3. Buffer creation copies the input List value into the Buffer value. Slicing
   and concatenation return new Buffers. No mutable backing storage or shared
   mutable view is exposed.
4. Buffers may be sent in Actor Messages and serialized by Distribution using
   the existing List and Record value rules. The 64 MiB limit applies before
   serialization and on receipt. Distribution must reject oversized payloads
   through its existing protocol error path.
5. The first Native ABI for binary operations accepts a borrowed pointer and
   length only for the duration of the call. BASIC owns the Buffer value and
   remains its sole lifetime owner. Native code must copy any bytes it retains
   beyond the call. Native results that outlive a call are copied into a new
   managed Buffer representation before returning to BASIC.
6. Native operations validate pointer/length pairs, reject lengths above the
   Buffer limit, and never use NUL termination. No raw pointer is exposed to
   BASIC code.
7. Hex and Base64 are pure synchronous transformations. Decode errors report
   a stable error code and the zero-based byte offset in the encoded input.
   Base64 decoding rejects invalid alphabet characters, misplaced padding,
   and non-canonical unused trailing bits.
8. UTF-8 conversion is explicit: encoding text produces a Buffer; decoding a
   Buffer returns `Result<String, Encoding_Error>` and rejects malformed UTF-8.
   Current Encoding functions call Native-backed Text helpers; embedded U+0000
   hits the existing NUL-terminated Native String boundary and raises a
   recoverable Runtime fault. A length-bearing Native String ABI is required
   before this edge can return `Encoding_Error` like other malformed input.

## Consequences

- The first implementation is straightforward and safe but stores each byte
  as a language integer. A dedicated compact Byte array may replace this
  representation later without changing the public `Buffer` operations.
- Buffer operations are immutable and may allocate. Their complexity and
  allocation behavior must be documented; APIs that produce a new Buffer are
  bounded by 64 MiB.
- Runtime support for compact Native byte slices may be added behind the
  borrowed-call ABI. That work must preserve Buffer ownership and GC safety.
- Binary File System and Network APIs must use Buffer and Actor protocols;
  they may not use String as a byte container.
- Buffer serialization requires conformance coverage for nested values,
  maximum lengths, and oversized-message rejection before Network v1 is
  considered complete.

## Implementation notes

The public compiler boundary can preserve `Record Buffer(List<Int>)` when the
source explicitly normalizes each byte through integer arithmetic before
constructing the Record. Base64 decoding must append bytes after branch joins;
appending to one List inside nested branches currently triggers MIR dominance
verification errors. The v1 implementation in `@std/encoding.bas` follows
these constraints without changing the compiler or treating String as a
binary container.

## Review checklist

- Are byte ranges and Buffer lengths checked at every public boundary?
- Are Native pointers borrowed only during the call and never retained?
- Are allocations bounded and ownership visible?
- Can Buffer cross Actor and Distribution boundaries without becoming
  mutable shared state?
- Are malformed UTF-8 and encoded input errors returned as stable Results?
- Does any API treat String as arbitrary binary data?
