# Path

`@std/path/path.bas` provides pure string operations over a portable logical
path syntax. It does not access the filesystem or read/change the process
working directory.

Paths use `/` as the only separator. A path is absolute when it starts with `/`
or has one scalar followed by `:/` (for example `C:/temp`). Backslashes are
ordinary characters. `join(base, child)` returns an absolute child unchanged;
otherwise it appends the child with one separator as needed. It preserves `.`
and `..` segments and does not canonicalize paths.

`basename` treats a trailing `/` as an empty final segment. `dirname` returns
the path before the last `/`, so `dirname("a/b/")` is `a/b`; it returns `.`
when no separator is present and `/` for a root-level name. `extension` returns
the final suffix including its dot; names beginning with a dot and names ending
with a dot have an empty extension.

All operations count Unicode scalar values through `@std/text`. A string with
embedded NUL raises the Text module's documented recoverable Native runtime
fault. The module does not treat OS-specific drive or UNC rules beyond the
portable `:/` prefix described above.
