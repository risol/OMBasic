# ADR-0020: Resolve imports strictly beneath the source root

Status: Accepted

## Context

Module imports resolve deterministically below one of two typed roots: the
project Source Root or the SDK Standard Library Root selected by `@std/`.
Portable, race-resistant support for safe in-root symlinks would require
different platform-specific resolution strategies.

## Decision

- Canonicalize and pin the source root, then reject symlink or reparse-point
  traversal for imported files.
- Require every requested path segment to use its exact filesystem casing.
- Reject an import alias that resolves to a different canonical path, even if
  its target remains inside the source root.
- Apply the same traversal, exact-case and symlink/reparse-point checks to each
  root independently. Root selection is explicit and never falls back.

## Consequences

- Each module path has one filesystem spelling.
- POSIX walks directory descriptors with `openat` and `O_NOFOLLOW`; Windows
  checks reparse points and compares the opened file's canonical path.
- Projects that need symlinks must materialize regular files under the source
  root before compilation.
