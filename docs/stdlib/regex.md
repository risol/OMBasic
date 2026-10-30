# Regex

Import `@std/regex/regex.bas` for `regex.find(pattern, input)`. It returns a
`Result` containing `Regex_Match(matched, start, end_index)`; the offsets are
zero-based Unicode scalar positions and `end_index` is exclusive. A valid
pattern with no match returns `Ok(Regex_Match(FALSE, 0, 0))`.

The engine parses each pattern into a Thompson NFA and simulates it without
backtracking. Its search policy chooses the earliest start and the shortest
match at that start. The implementation accepts UTF-8 literals, `.`, `^`, `$`,
groups, alternation, `*`, `+`, `?`, bracket classes and ranges, and escapes
`\n`, `\r`, `\t`, `\f`, `\v`, `\d`, `\s`, `\w` and their uppercase
complements outside classes. `\w` is ASCII letters, digits, and underscore;
`\s` is ASCII whitespace. Dot matches any scalar except LF.

Backreferences, lookaround, named groups, Unicode property escapes, word
boundaries, and octal escapes are unsupported and return
`Regex_Error("unsupported", byte_offset)`. Malformed patterns return
`invalid_pattern`; malformed UTF-8 input returns `invalid_utf8`. Limits are
4096 pattern bytes, 1 MiB input bytes, nesting depth 128, 8192 NFA instructions,
and 16 million simulation steps. Limit failures return `resource_exhausted`.

Capture groups are for grouping only; capture extraction and replacement are
not part of this API.
