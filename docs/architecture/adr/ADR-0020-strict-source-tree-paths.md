# ADR-0020: Resolve imports strictly beneath the source root

Status: Accepted

## Context

Module imports must resolve deterministically below one canonical source root.
Portable, race-resistant support for safe in-root symlinks would require
different platform-specific resolution strategies.

## Decision

- Canonicalize and pin the source root, then reject symlink or reparse-point
  traversal for imported files.
- Require every requested path segment to use its exact filesystem casing.
- Reject an import alias that resolves to a different canonical path, even if
  its target remains inside the source root.

## Consequences

- Each module path has one filesystem spelling.
- POSIX walks directory descriptors with `openat` and `O_NOFOLLOW`; Windows
  checks reparse points and compares the opened file's canonical path.
- Projects that need symlinks must materialize regular files under the source
  root before compilation.
