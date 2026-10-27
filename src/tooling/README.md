# Compiler tooling

The tooling layer depends on the public syntax and semantic APIs. It owns the
formatter, structured compiler diagnostics, in-memory module interface views,
type display, and the `ombasic` developer executable. LSP requests go through
the same compiler pipeline as `ombasic check`; the Python server only handles
JSON-RPC framing, document synchronization, and LSP position conversion.

## Commands

```text
ombasic version
ombasic fmt [--write|--check] <file.bas>
ombasic check --root <source-root> <entry.bas> [--diagnostics=json]
ombasic run --root <source-root> <entry.bas> [--entry function]
ombasic build --root <source-root> <entry.bas> [-o output.ombc]
ombasic check-omi <file.omi>
ombasic bind --header <file.h> --library <path> --output <file.omi> [-I dir]
ombasic lsp --root <source-root>
```

`fmt` requires lexically and syntactically valid source. It normalizes keyword
spacing, indentation, blank lines, and line endings to LF while preserving
regular and documentation comments. `--write` updates the named file;
`--check` returns nonzero when formatting would change it.

`check --diagnostics=json` emits a JSON object with a diagnostics array. Each
item has `stage`, stable `code`, `severity`, `message`, `path`, and a half-open
`range`. Lines and UTF-8 byte columns are one-based, matching
`docs/development/diagnostics.md`. Related resolution locations are included
when the compiler supplies them. `--include-inferred` is an extension used by
the LSP to obtain source-spanned inferred types from the compiler model.

`run` checks and compiles the project to bytecode, then executes the `main`
function (or the function named by `--entry`). It prints scalar results to
stdout; a Unit result produces no output. `build` performs the same compiler
validation and writes a serialized OMBC image, defaulting to `out.ombc` when
`-o`/`--output` is omitted. The artifact is portable bytecode, not native
machine code. The current CLI does not load OMI native bindings at run time.

## In-memory module interfaces

`om_interface_cache_build` selects exported symbols and exported Actor/Message
protocol entries from the completed semantic model. Each module records its
source content hash and a structural interface hash over exported names,
types, effects, serializability, and Actor protocol pairs. This cache is
in-memory only. Its names, paths, type IDs, and schemes borrow the project,
resolution, and inference results; destroy it before those results.

`om_tooling_type_to_string` renders inferred types for hover and inlay hints.
Unresolved type variables are shown with analysis-local IDs and are not stable
interface names.

## LSP and native binding

The early LSP supports diagnostics, hover, and type inlay hints. It handles
open, close, full-document changes, and ranged changes. Every analysis uses the
compiler's lexer, parser, resolver, type inference, and semantic validation;
the LSP does not implement a second grammar or type checker. Imported modules
are read from the source root, while the open entry module is analyzed from the
editor's current buffer. Analysis currently runs as a compiler subprocess per
request rather than using an incremental process-wide cache.

`ombasic bind` asks Clang for its JSON AST and emits scalar and single-pointer
function signatures as OMI. The first pass excludes static and variadic
functions and skips unsupported aggregate/callback declarations. Generated
metadata uses conservative placeholders: `EFFECT NATIVE`, `BLOCKING`,
`BORROWED` for pointer parameters, and `RETURNS STATIC` for pointer results.
Review those choices against the C API before using the `.omi` file. Run
`ombasic check-omi` to validate the generated syntax and annotations.
