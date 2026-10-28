# Compiler tooling

The tooling layer uses public syntax, semantic, and Native metadata APIs. It
owns the formatter, structured diagnostics, module interface views, type
display, and the `ombasic` executable. The Python LSP layer handles JSON-RPC
framing and editor position conversion while the compiler performs analysis.

## Commands

```text
ombasic version
ombasic fmt [--write|--check] <file.bas>
ombasic check --root <source-root> <entry.bas> [--native file.omi] [-L dir]
ombasic build --root <source-root> <entry.bas> [--native file.omi] [-L dir] [--emit=hir|mir|llvm-ir|asm|obj] [-o output]
ombasic run --root <source-root> <entry.bas> [--native file.omi] [-L dir] [--entry function]
ombasic check-omi <file.omi>
ombasic bind --header <file.h> --library <path> --output <file.omi> [-I dir]
ombasic lsp --root <source-root>
```

`check` loads Native metadata supplied with `--native` and type-checks native
declarations. `build` and `run` lower the program to LLVM object code, link it
with the OMBasic runtime, and link each declared native library. Pass `-L dir`
to add a native library search directory. `build --emit=hir|mir|llvm-ir|asm|obj`
writes the requested compiler artifact without linking an executable; `-o`
selects its output path, otherwise it writes `ombasic.<suffix>` in the current
directory.

`fmt` requires valid lexical and syntactic input. It normalizes spacing,
indentation, blank lines, and line endings while preserving comments.
`check --diagnostics=json` emits stable diagnostic codes and source ranges.
`--include-inferred` and `--include-interfaces` expose semantic data used by
the LSP.

`ombasic bind` asks Clang for its JSON AST and emits supported C declarations
as `.omi` Native metadata. Review inferred ownership/effect annotations
against the C API, then run `ombasic check-omi`. Metadata parsing lives in the
`native_metadata` capability; Runtime dynamic symbol loading has been removed.
