# Language conformance fixtures

These fixtures verify user-observable behavior against the normative rules in
[`OMBasic-Language-Standard.md`](../../OMBasic-Language-Standard.md). Put source
files under directories named for their language area and add each executable
case to `manifest.json`.

Manifest schema version 2 gives every case a stable kebab-case ID, one or more
`OM-SS.RR` Rule IDs, a case `kind`, source or project entry, optional platform
and target constraints, tags, and an expected result. Compile failures assert
the stable diagnostic code and primary span. Span lines and UTF-8 byte columns
are one-based; the end column is exclusive. Optional diagnostic `text` must
match the token selected by that span.

Supported kinds are:

- `compile`: compile success or failure;
- `run`: compile successfully and compare process exit code and optional exact
  stdout/stderr;
- `multi_file`: compile (and optionally run) an entry in a project root;
- `multi_process`: run a scenario script under `runner/scenarios/`.

Tags include `lexer`, `parser`, `semantic`, `type`, `effect`, `actor`,
`runtime`, `native`, `distribution`, `diagnostic`, `windows`, `linux`, and
`slow`. Release CTest runs every non-slow case supported by the current
platform. Slow cases require the runner's `--include-slow` option.

`validate_manifest.py` validates schema, fixture paths, and diagnostic spans.
`tools/conformance_check/check.py` extracts normative Rule IDs from the
Standard, rejects unknown references, requires 100% Rule coverage, and writes
the coverage report. `runner/conformance_runner.py` invokes `ombasic check`
and `ombasic run` for compiler-facing cases.

Module fixtures cover file-based imports, directory Facade conventions,
current-Module-relative `./` paths, public re-exports, private-member hiding,
name conflicts, and dependency cycles. Re-export cases also check that
interface metadata exposes the Facade surface while preserving the original
declaration location.
