# ADR-0007: Use strict source-tree path resolution

Status: Accepted

## Context

Module imports must resolve deterministically below one canonical source root. The development plan requires preventing symlink escapes and permits a strict policy that rejects source-tree symlinks. Portable, race-resistant support for safe in-root symlinks would require different platform-specific resolution strategies.

## Decision

The reference implementation canonicalizes and pins the source root, then rejects symlink or reparse-point traversal for imported files. Every requested path segment must use its exact filesystem casing. An import alias that resolves to a different canonical path is rejected even when its target is still inside the source root.

## Consequences

- Import resolution has one filesystem spelling for each module path.
- POSIX walks directory descriptors with `openat` and `O_NOFOLLOW`.
- Windows checks reparse points and compares the opened file's canonical path to the requested path.
- Projects that need symlinks must materialize regular files under the source root before compilation.
