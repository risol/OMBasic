# LLVM Native Backend

OMBasic uses LLVM 23.x to turn verified MIR into a platform object file. The
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

The CI Hello World gate exercises the installed native build path:

```basic
ACTION main()
    print("Hello, OMBasic!")
END ACTION
```

The backend passes a hidden Runtime Context through direct and indirect
functions and emits shadow-root frames for managed values. It lowers scalar
and managed values, `Result<T, E>`, checked operations, collections, Actor
handlers, Native FFI calls and callback trampolines, source debug information,
and static descriptors. Native execution and distribution are covered by the
Runtime and CLI integration suites.

The backend implementation is split into private modules for module setup,
type lowering, functions, control flow, Runtime calls, GC roots, descriptors,
Actors, FFI, callbacks, debug information, and output. A single backend context
owns LLVM state and receives the explicit `Om_Target` for each compilation.
The target module selects ABI classification; the backend does not infer C ABI
or debug format by re-parsing the triple. See the [target model](target_model.md).

LLVM is a compiler-time dependency. Generated programs statically link the
OMBasic Runtime and foundation archives; they do not link LLVM-C or load
`LLVM-C.dll` at runtime.
