# Source capability

The Source capability owns immutable, source-root-relative module text, byte-based spans, and line maps. Its public API depends only on Foundation.

`Om_Source_Text` owns copies of its UTF-8 canonical path, arbitrary source bytes, and a line-start table. It preserves malformed source bytes for the lexer to diagnose. The default per-file limit is 16 MiB and callers may choose a smaller or larger limit, bounded by the 32-bit span-offset representation.

Line and byte-column locations are one-based. LF and CRLF both create a line start after LF; a lone CR remains on the current line so the lexer can report it as an invalid source character. Content hashes are deterministic non-cryptographic FNV-1a values for local change detection, not security decisions.

The loader is responsible for canonicalizing paths and proving that resolved imports remain inside the configured source root.

`Om_Source_Root` must be zero-initialized before `om_source_root_init`. It pins the canonical directory for its lifetime. POSIX loads walk each path segment with `openat` and `O_NOFOLLOW`, and check directory entries with exact byte casing. Windows loads reject leaf reparse points and compare the opened file's canonical path with the requested path using case-sensitive ordinal comparison; the pinned root cannot be renamed while it is in use. A file whose spelling differs only by case is treated as not found.

Import path syntax is validated before filesystem access. Paths must be relative slash-separated paths; every directory and the final file stem must match lower_snake_case, and the final extension must be exactly .bas.
