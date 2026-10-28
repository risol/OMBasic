# LLVM Native Backend

OMBasic uses LLVM 23.1.4 to turn verified MIR into a platform object file. The
backend owns OMBasic language semantics in HIR/MIR; LLVM handles target code
generation and object emission.

The current compiler path is:

```text
.bas -> frontend and semantic checks -> HIR -> specialization -> MIR
      -> LLVM IR -> .obj/.o -> platform linker -> native executable
```

`ombasic run` performs this pipeline, starts the executable, and returns its
exit status. `ombasic build` leaves a standalone executable at the path given
by `-o` or `--output`. Temporary object files are removed after linking.

The Phase 6 Hello World gate remains supported:

```basic
ACTION main()
    print("Hello, OMBasic!")
END ACTION
```

The backend passes a hidden Runtime Context through direct and indirect
functions and emits shadow-root frames for managed values. Runtime-backed
Strings, Record/Message descriptors and fields, Lists, Maps, and `print` are
implemented for the current non-Actor path. It also lowers Int, Float, Bool,
branches, and loops. `Result<T, E>` uses a concrete tagged aggregate; `TRY` is
lowered to explicit branches and returns. Checked integer arithmetic, division,
collection indexing, and invalid range steps report runtime faults through the
root boundary with source locations. Direct scalar C calls, CString
conversion, managed `CPointer` calls, and opaque by-value Native Record calls
are supported through `.omi` metadata for Linux SysV AMD64 and Windows Win64
x64. Callback parameters, Actor execution, and distributed runtime behavior
remain in later phases.

LLVM is a compiler-time dependency. Generated programs statically link the
OMBasic Runtime and foundation archives; they do not link LLVM-C or load
`LLVM-C.dll` at runtime.
