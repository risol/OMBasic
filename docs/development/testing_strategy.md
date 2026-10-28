# Testing strategy

Tests are organized by capability and follow the current migration stage:

1. Foundation unit tests cover allocation, arenas, vectors, IDs, and boundary
   behavior.
2. Source, lexer, parser, and module graph tests cover valid input, malformed
   input recovery, diagnostics, and import graphs.
3. Semantic and compile-fail tests cover names, types, effects, Actor
   protocols, and serialization.
4. Native metadata tests cover `.omi` parsing, ownership/blocking declarations,
   malformed input, and source spans.
5. HIR tests cover deterministic lowering and invariants. MIR tests cover CFG
   construction, deterministic dumps, dominance, call signatures, verifier
   rejection, and a source-to-specialization-to-MIR pipeline for scalar calls,
   branches, returns, while loops, signed numeric ranges, escaped string
   literals, interpolation, List/Map literals and indexing, List/Map
   `FOR EACH` with loop transfers, function-valued indirect calls,
   Record/Message fields and updates,
   `Result`/`TRY`/`SELECT`, and Null constants/comparisons.
6. LLVM tests verify generated IR, target object headers with `llvm-readobj`,
   checked arithmetic and Runtime call lowering, debug information, and the
   absence of LLVM or OMBasic Runtime imports from standalone executables.
7. Native integration tests link and execute standalone programs on Windows
   and Linux, covering language behavior, GC roots, Actors, Native FFI and
   callbacks, system services, and distributed Actor lifecycles.
8. Runtime tests cover roots and collection, Actor isolation and scheduling,
   finalization, blocking and timer services, wire bounds, and TCP delivery.

The Lexer suite runs deterministic arbitrary-byte and string inputs and checks
token/diagnostic span bounds. The Parser suite compares normalized AST output
with golden files and checks recovery and nesting limits. Every parser or
decoder that accepts external data must include malformed-input coverage.

CTest is the local test entry point; the release matrix contains 23 tests.
Sanitizer profiles are separate build configurations. Linux Clang
ASan/UBSan runs the full suite; TSan is used for the concurrent Runtime and
foreign-thread callback paths. With Clang, configure
`OM_SANITIZER=address-undefined` and `OM_BUILD_FUZZERS=ON` to build the nine
libFuzzer targets for import paths, source lexing/parsing, `.omi` metadata,
distribution frames and payloads, HIR/MIR verifier inputs, and verified-MIR
LLVM object emission. See `tests/fuzz/README.md` for bounded smoke-run
commands. The distribution integration test launches two native processes on
localhost. No VM/OMBC behavior is part of the target architecture.
