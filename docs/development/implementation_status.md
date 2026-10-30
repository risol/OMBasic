# Native AOT implementation status

## Query v0.1 — implementation in progress

- The compiler supports eager `List<T>` Query expressions with ordered clauses,
  Query-local range bindings, typed filters/take/order/projection, purity checks,
  typed HIR, a verified temporary QIR plan, fused scan lowering, stable ordering,
  bounded Top-K selection, and delayed ordered projection. Query expressions
  compile inside Actor `RECEIVE` handlers without scheduler changes.
- Query diagnostics E1201–E1204 identify invalid source, WHERE, TAKE, and order
  key types and include actual/expected types. Language semantics and Actor
  responsiveness guidance are in `docs/language/query.md`, ADR-0042, and the
  language standard.
- Windows Release Query runtime conformance, all Linux Release CTest cases
  except the pre-fix reserved-keyword audit, and the targeted Windows/Linux
  syntax, semantic, HIR, MIR, runtime-list, and formatter tests have passed.
  The reserved-keyword audit now passes after adding the Query keywords to the
  language-standard table. Sanitizer, parser-fuzzer, and performance benchmark
  gates remain outstanding; this feature is not yet complete.

## Foundation libraries — implementation in progress

- Added Number parsing/formatting, UTF-8 String/Buffer conversion, binary File
  System requests, URI, SHA-256/CRC-32, Environment/Platform, typed IP and
  socket addresses, DNS, TLS, HTTP/1.1 request/response handling and Client
  Actor, typed-empty collection inference, UUID, and CSV. Public contracts,
  resource bounds, and module imports are described in `docs/stdlib/` and
  ADR-0041.
- HTTP Client Actor loopback coverage verifies HTTP over TCP. A Linux local
  HTTPS scenario now verifies certificate and host validation, HTTP request
  delivery over TLS, and response parsing against the local CA fixture.
- Bounded gzip and zlib-wrapped DEFLATE are now available over Buffer with
  checksum validation and a decoder fuzzer. Conformance covers local/external
  gzip data, optional gzip headers, empty streams, round trips, bad checksums,
  and malformed streams. The seeded compression decoder fuzzer passes 200
  ASan/UBSan runs. Regex v1 now provides bounded Thompson-NFA search with
  Unicode-scalar offsets, documented syntax, conformance, and a seeded
  ASan/UBSan fuzzer. SQLite 3.54.1 is bundled in the Runtime and exposed
  through a per-connection Database Actor with bounded execute/query
  operations and JSON row output. HTTP Client Actor now reuses same-origin
  keep-alive connections and has explicit close lifecycle coverage. ZIP/TAR
  archives and direct HTTP response parser fuzzing remain open items.
- The complete Windows Release CTest suite passes (38/38). The complete Linux
  Release CTest suite passes (39/39), including the HTTPS scenario and Runtime
  conformance and compression; the Runtime conformance test completes in
  158.91 seconds with the latest SDK copy in the single-core validation
  container. SDK relocation checks both the standard
  library `.bas` and module `.omi` digests.
- Linux Regex parser fuzzing passes a 500-run ASan/UBSan smoke test. The full
  Linux ThreadSanitizer CTest suite passes (39/39) when run with `setarch -R`
  in a container with seccomp disabled; this includes Runtime conformance and
  the 2,000,000-message Actor stress test. A direct HTTP response-parser fuzz
  target remains open; `http_response_fuzz.bas` is a deterministic
  conformance stress case, not a libFuzzer target.

The migration follows [OMBasic-Native-AOT-Refactor-Plan.md](../../OMBasic-Native-AOT-Refactor-Plan.md).
The source and architecture baseline is commit
`ecbbacc5fb235c4a706a9c4eeb31bc7b41dec299` on branch
`codex/native-aot-refactor`.

## Phase 0 — Architectural reset: complete

- Recorded the last VM baseline commit and created the migration branch.
- Adopted Native AOT as the only execution backend in ADR-0003.
- Defined Native representation, managed-pointer policy, shadow roots, Actor
  handler ABI, descriptors, LLVM version policy, and static-link model in the
  Runtime ABI and HIR/MIR design documents.
- Replaced the architecture overview and dependency graph with the Native AOT
  boundaries.

## Phase 1 — Destructive VM / OMBC removal: complete

- Removed Bytecode, VM, VM-owned Value/Heap, VM distribution integration,
  compiler/VM Native binding, and VM-only tests and ADRs.
- Rebuilt the CMake graph around frontend, semantic, metadata, tooling, and
  independent protocol/service capabilities.
- At this phase, `build` and `run` only completed source/semantic validation;
  Phase 6 below records the later native backend milestone.
- Windows GCC/Ninja configure and build succeeded; CTest passed 14/14; the
  architecture checker passed.

## Phase 2 — Native metadata capability: complete

- Moved `.omi` parser/project loader and public contracts to
  `src/native/metadata` and `include/ombasic/native/metadata`.
- Removed runtime `libffi` and dynamic-library dispatch. Semantic resolution
  consumes metadata contracts only.
- Native metadata parser/project tests, malformed-input corpus, CLI, and the
  full 14-test CTest suite pass.

## Phase 3 — HIR: complete

- Added typed HIR storage, AST/semantic lowering, deterministic dumps, and a
  verifier for structure, symbols, types, effects, Actor protocols, message
  serializability, Actor-state access, and `TRY` input types.
- Deterministic lowering is covered by positive single-module and multi-module
  semantic fixtures; six normalized golden dumps cover language entities,
  control flow, `Result`, Actor operations, system Actor messages, module
  imports, and Native declaration references. Verifier corruption checks cover
  `TRY`, function effects, `SEND` protocol, and Actor-state placement.
- HIR and the full 15-test CTest suite passed before Phase 4 work began; the
  architecture check passes as part of CTest. The HIR is a structured tree;
  CFG terminators are represented and verified in MIR.

## Phase 4 — Specialization: complete

- Added canonical type-argument matching, a deterministic worklist, reachable
  roots for Actions, receive handlers, and Actor-state initializers, plus
  diagnostics for calls whose generic arguments cannot be resolved.
- Added coverage for concrete `identity(int)` and `identity(string)` instances
  reachable from `main`, and unresolved calls in the broader inference fixture.
  The focused HIR gate and full 15-test CTest suite pass. Specialization now
  returns deduplicated concrete List/Map/Result type IDs. Static Runtime
  descriptor records and callbacks are emitted with the Runtime in later phases.

## Phase 5 — MIR: complete

- Added MIR function/block/value/instruction IDs, a CFG construction API with
  block parameters and explicit terminators, deterministic MIR dumps, and
  verification for structural ranges, concrete signatures and values,
  direct-call signatures, edge argument types, dominance, effects, operand
  types, and field ownership/layout.
- HIR-to-MIR covers concrete scalar Function/Action instances, direct and
  indirect calls, assignments, returns, `IF`, `WHILE`, numeric `FOR`, and List
  and Map `FOR EACH` loops with `BREAK` and `CONTINUE`. It lowers escaped and
  interpolated strings (including Null), List/Map construction and indexing,
  Record/Message construction and field access, Record `WITH`, Result
  construction, `TRY`, and `SELECT`. MIR owns UTF-8 string bytes and records
  nominal field layouts so the verifier can check field reads, writes, copies,
  constructor arity, and argument types. The source-to-MIR fixture covers these
  paths and deterministic dumps. `AND` and `OR` use short-circuit CFGs; the
  MIR goldens cover both logical operators. The full 16-test CTest suite passed
  at this phase.
- Actor and Native operation lowering is intentionally deferred to their
  dedicated runtime/FFI phases.

## Phase 6 — Minimal LLVM Native Backend: complete

- Added LLVM 23.1.4 C API lowering from verified MIR to native object files.
  The initial backend covers scalar values, calls, control flow, checked
  integer arithmetic, and a `print(String)` console helper.
- Connected `ombasic run` and `ombasic build` through source analysis, HIR,
  specialization, MIR verification, LLVM object emission, and native linking.
  `ACTION main()` can be selected by default; `run --entry <callable>` can
  select another top-level action.
- Split target ABI descriptions, compiler orchestration, and platform linking
  into the `ombasic_target`, `ombasic_compiler`, and `ombasic_linker` targets.
  The CLI coordinates these modules; the linker receives Runtime archive paths
  and links generated programs without depending on Runtime implementation
  code.
- The standard build matrix is Windows `clang-cl.exe` with `lld-link.exe`
  (MSVC ABI), and Linux `clang` with `ld.lld` selected through
  `-fuse-ld=lld`, with CMake and Ninja on both platforms. Windows uses the
  MSVC toolset and Windows SDK headers and libraries; MinGW is not required.
  CMake rejects other compiler families on Windows and Linux.
- On Windows, the clang-cl/LLD build compiles the compiler, links generated
  COFF executables with the static MSVC/UCRT libraries, and does not load LLVM
  at runtime. Hello World runs, and the full clang-cl CTest suite passes
  (19/19).
- Linux validation uses the extracted LLVM package at
  `F:\LLVM-23.1.4-Linux-X64`, mounted read-only in Docker as `/opt/llvm`, with
  Clang, `ld.lld`, CMake, and Ninja. No second LLVM download is needed. The
  Release build and Hello World run succeeded;
  the full 19-test CTest suite passes, including `tooling_cli` and the
  two-million-message `native_actor_stress` case.

## Phase 7 — Native Object Model + GC: complete on Windows and Linux

- Added the C-callable Native Runtime ABI for execution contexts, descriptors,
  shadow-root frames, Strings, Records/Messages, Lists, Maps, printing, and
  collection statistics. Null and Unit are represented without payload.
- Added a non-moving mark-and-sweep heap with descriptor-driven field tracing,
  optional descriptor hooks, and managed-reference tracing through Record,
  List, and Map contents. The root Action uses the same context/heap API
  planned for later Actor-local heaps.
- Collection thresholds grow with the live heap, and queued mailbox objects
  are marked directly from their owned queue entries. This prevents repeated
  small-heap collections from rescanning a large transit mailbox quadratically.
  A focused runtime test verifies threshold growth while preserving rooted
  objects.
- Generated-native integration covers managed parameters and nested
  Record/List/Map graphs across allocation safepoints, plus Actor state and a
  transferred nested Message graph. Collection tests also cover live and
  unreachable cycles.
- LLVM now passes the hidden Context through direct and indirect OMBasic calls,
  roots managed MIR values for each native function, and calls the Runtime for
  String creation/formatting/concatenation/printing and Record/List/Map
  operations. Concrete structural descriptors are emitted into the native
  object, and the CLI statically links the Runtime and foundation archives.
- Windows clang-cl/LLD build and all 19 CTest tests pass. Coverage includes
  nested root-frame order, cyclic collection, Null/Unit containers, string-key
  equality, references retained through containers, generated Strings
  surviving a GC safepoint, interpolation, Record and Message field access,
  List/Map indexing and iteration, direct and indirect calls, recursion,
  floating-point arithmetic, Result/TRY matching, and the former non-Actor
  integration fixture's expected output.
- The former non-Actor integration fixture has been ported to Native CLI
  coverage, including Records, `WITH`, Lists/Maps, numeric and collection
  loops, Result propagation and pattern matching, Message construction and
  field reads, string interpolation, recursive and indirect calls, and Float
  arithmetic. Runtime fault cases remain covered by Phase 8.

## Phase 8 — Result / TRY / Runtime Faults: complete

- Lowered `Result<T, E>` as a concrete tagged LLVM aggregate and implemented
  `TRY` using the existing MIR branch-and-return control flow. Managed values
  nested in active Result payloads are included in per-function shadow roots.
- Added checked fault paths for integer overflow, division by zero, list/map
  index failures, missing map keys, and zero numeric-range steps. Runtime
  boundaries report static source spans and restore the root chain after a
  non-local fault. Nested boundaries support both the root entry and future
  Actor handlers.
- Native CLI coverage verifies `Result` error propagation, a managed Result
  payload surviving a GC safepoint, divide-by-zero, overflow, collection
  bounds, zero range steps, and file/line fault locations. Runtime tests verify
  root restoration and nested fault boundaries. `TRY` in a Receive lowers its
  Error branch to a contained Actor failure, matching the language standard;
  watchers observe the normal `FAILED` termination path.
- The Windows clang-cl/LLD build and Phase 8 runtime/CLI tests pass. Phase 7's
  former non-Actor integration fixture is covered by the Native CLI fixture
  summarized above.

## Phase 9 — Native Actor Runtime: complete on Windows and Linux

- Added MIR Actor descriptors for state initializers, receive handlers, and the
  built-in `Actor_Down` message layout. The MIR verifier checks Actor opcode
  operand/result types and spawn targets.
- The LLVM backend emits native Actor descriptors and callback adapters, lowers
  spawn/self/send/watch/stop calls, and emits the Program descriptor. The native
  entry wrapper starts the bounded Runtime worker pool and waits for outstanding
  messages after the entry Action returns.
- Runtime Actor state initialization now runs inside a recoverable fault
  boundary. A native CLI integration case verifies spawn, message delivery,
  state mutation, printing from a receive handler, and stop. A second CLI case
  verifies a `SELF` reference sent through a message and a reply round trip.
  Another case verifies watch delivery of `Actor_Down` after a failed handler
  and checks the `FAILED` reason; initializer overflow is also contained by its
  fault boundary. Additional CLI coverage verifies self-send countdowns,
  independent state for two instances of the same Actor, and idempotent WATCH
  after a target has terminated.
- Runtime Actor tests verify one Actor never runs overlapping handlers,
  separate Actors can run concurrently, STOP discards queued mailbox messages,
  WATCH emits one terminal notification, and a two-million-message workload
  drains successfully. On termination, the Runtime detaches and destroys the
  Actor heap before publishing completion to waiters; a Native pointer
  finalizer test verifies reclamation is complete when `actor_system_wait`
  returns. The workload completes in 4.72 seconds on Windows and 43.60 seconds
  in the single-core Linux Docker environment after mailbox GC scaling was
  corrected.
  Focused MIR, Runtime Actor, and CLI tests pass on Windows clang-cl/LLD. The
  complete Windows CTest suite passes (19/19), and the architecture checker
  passes.
- The Windows linker driver trace confirms `clang-cl.exe -fuse-ld=lld`
  invokes `lld-link.exe` with the MSVC and Windows SDK library directories.
- The Linux Clang/LLD build and CLI integration pass. Its full 19-test CTest
  suite also passes, including the actor stress workload.

## Phase 10 — System Actors: complete on Windows and Linux

- Console writes run on the blocking pool, preserve the supplied text without
  adding a newline, and return `Console_Write_Completed(Result<Unit, String>)`.
- File_System copies UTF-8 paths and write contents into owned native task data;
  workers perform UTF-8 text reads/writes, and mailbox completions carry
  `Result<String, String>` or `Result<Unit, String>`. Timer keeps a deep-cloned
  message in a task-owned heap and sends it to the recipient when the timer
  expires.
- Runtime operation counts keep `actor_system_wait` open until asynchronous
  completions have been enqueued and processed. Shutdown cancels pending timers
  and queued blocking work before joining service threads.
- CLI integration covers Console, File_System write/read, Timer delivery, and a
  File_Read failure propagated by `TRY` in a Receive and observed as
  `Actor_Down(Actor_Down_Reason.failed)`. The Windows clang-cl/LLD full CTest suite passes (19/19).

## Phase 11 — Direct Native FFI: complete on Windows and Linux

- `check`, `build`, and `run` accept repeatable `--native file.omi` options;
  `build` and `run` also accept `-L dir` for native library search paths.
- `.omi` parses callback signatures and
  `CALLBACK <parameter> FOREIGN_THREAD MESSAGE <Message>` bindings. It rejects
  missing bindings and non-`CVoid` callback results; metadata tests cover both
  valid and invalid forms. Type inference resolves each binding to one unique
  OMBasic `MESSAGE`, checks the callback payload field count and types, and
  requires registration from an Actor `RECEIVE` that accepts the bound Message.
  CLI cases cover valid bindings, missing or unaccepted Messages, unsupported
  pointer payloads, field-count/type mismatches, and calls outside `RECEIVE` on
  Windows and Linux. Generated C ABI trampolines post supported scalar payloads
  to the Actor mailbox from foreign threads. Registrations are scoped to the
  Native call; unregister waits for in-flight ingress, and the Native library
  must join its callback threads before returning. The concurrent two-Actor
  callback fixture passes on Windows and Linux.
- MIR carries the resolved native symbol to LLVM. Direct C calls now lower
  fixed-width integers, C integer aliases, `CSize`, `CBool`, `CFloat`, and
  `CDouble`, including target-aware `CLong` width and narrow integer extension
  attributes. Native libraries link through the selected platform linker.
- CString parameters reject embedded NUL and use stack-backed, NUL-terminated
  copies for the duration of the call. CString results are copied into managed
  UTF-8 strings; borrowed results are copied before temporary inputs expire,
  and owned results invoke their declared release function after the copy
  attempt.
- The Windows CLI fixture links a C static library and executes calls using
  `CInt`, `CInt8`, and `CUInt8`, including signed and unsigned narrow values.
  It also covers borrowed and owned CString results, rejects embedded NUL, and
  repeats CString calls 100,000 times to cover stack cleanup. The clang-cl/LLD
  build succeeds and the full CTest suite passes (21/21).
- Managed `CPointer` values wrap foreign addresses. Borrowed and static
  parameters are unwrapped only for the call; owned parameters disarm their
  wrapper before transferring ownership. Owned results receive a finalizer,
  borrowed results retain their CPointer owner, and runtime tests cover explicit
  take, automatic release, and owner tracing.
- Native Records are opaque nominal values that can be passed to or returned
  from Native functions. Separate Win64 x64, SysV AMD64, AAPCS64, and Darwin
  ARM64 classifiers handle scalar field layouts, register coercions, indirect
  parameters, and `sret`. AAPCS64 tests cover HFA arrays and partial aggregate
  chunks; Linux HFA parameters use `alignstack(8)`, while Darwin ARM64 does not.
  The C roundtrip fixture covers mixed integer/float fields, floating-point
  register classes, pointer fields, and a 24-byte aggregate. The end-to-end
  Native CLI test passes under both Linux `clang + ld.lld` and Windows
  `clang-cl + lld-link`.
- A C++ `extern "C"` fixture also returns and accepts a two-double Native
  Record by value; the end-to-end CLI gate passes on Windows and Linux.
- Native Records remain opaque in BASIC source, and nested records are
  unsupported. Callback payload support is currently limited to integer
  aliases, `CBool`, `CFloat`, and `CDouble`.
- `BLOCKING` Native calls are rejected in Actor `RECEIVE` handlers with MIR
  diagnostic `E1104`; the CLI compile-fail case passes on Windows and Linux.

## Phase 12 — Distribution Native Integration: complete on Windows and Linux

- Added a Native Runtime adapter over the existing v3 TCP protocol and
  handshake. It uses generated Program/Message descriptors and program/interface
  fingerprints; it does not consume VM or bytecode data.
- Message fingerprints recursively include canonical module/entity identity
  and ordered field type fingerprints. Actor protocol fingerprints include
  their canonical identity and accepted Message schemas. The interface
  fingerprint is separate from the program fingerprint; CLI integration checks
  repeatability and schema-change detection.
- Added stable peer `Node_Ref` handles, remote ActorRef proxies, descriptor-based
  Message serialization, remote spawn, send, watch acknowledgements, and
  `Actor_Down` delivery. Reader threads use cloned Runtime contexts and root
  decoded managed values until mailbox enqueue.
- `runtime_distribution_native` launches two native processes on localhost.
  The integration covers handshake, remote spawn, String and ActorRef Message
  fields, a response, Watch, and normal Actor termination on Windows and Linux.
- The same integration verifies `Actor_Down_Reason.node_unreachable` watcher delivery. Protocol
  coverage checks ordered at-most-once sequential frames and a 64 KiB nested
  Record/List/Map payload against accepting and rejecting budgets.
- `CPointer` is rejected by the wire serializer. Failed sends are not retried;
  peer authentication remains outside the protocol contract.

## Phase 13 — CLI / Tooling Completion: complete on Windows and Linux

- `version`, `fmt`, `check`, `build`, `run`, `bind`, `check-omi`, and `lsp`
  remain available with the Native AOT pipeline.
- `build --emit=hir|mir|llvm-ir|asm|obj` writes compiler artifacts without
  linking an executable. CLI coverage inspects emitted object formats with
  `llvm-readobj`, checks LLVM lowering patterns and executable imports, verifies
  formatter idempotence and diagnostics, and exercises LSP hover/inlay requests.

## Phase 15 — Release Gates: complete on Windows and Linux

- The Windows clang-cl/LLD Debug and Release suites and the Linux Clang 23.1.4
  Release suite each pass 23/23, including checked-size builtin fallback, the
  target ABI model, AAPCS64/Darwin ARM64 ABI classifier checks, the two-process
  distribution integration, and the two-million-message Actor stress test.
- Linux Clang 23.1.4 ASan/UBSan passes the complete 23-test CTest suite,
  including generated Native executables. Linux Clang TSan also passes the
  complete 23-test suite in an isolated container with ASLR disabled.
- The LLVM PassBuilder C API runs `default<O0>` for Debug and `default<O2>` for
  optimized configurations. The program entry frame remains available to
  debuggers while other O2 inlining remains enabled.
- Runtime lookup storage uses each value's LLVM ABI size and alignment. The
  system-file-read Actor integration exercises a `Result<String, String>`
  payload larger than the former scalar-sized temporary. Checked-size helpers
  use compiler builtins when `<stdckdint.h>` is unavailable, with an MSVC
  arithmetic fallback.
- Added nine optional Clang libFuzzer targets for import paths, the source
  lexer/parser, `.omi` metadata, distribution frame/message decoding, HIR and
  MIR verifier inputs, distribution payload serialization, and LLVM object
  emission from verified MIR and descriptor-driven native Message codec
  round trips. Eight targets build with the extracted Linux LLVM package and
  pass a 10,000-run ASan/UBSan smoke pass; backend object emission passes a
  100-run smoke pass.
- The current fuzzers do not execute arbitrary generated native code.
- The foreign-thread callback integration passes under Linux Clang 23.1.4
  ASan/UBSan and TSan. Native Record by-value calls pass the current target-family
  ABI classifiers; BASIC-level construction, field access, and nested Native
  Records remain unsupported.

## Phase 14 — Native Debugging: complete

- MIR functions retain their source-level names for native debug metadata.
- The LLVM backend now emits a compile unit, source files, function scopes, and
  per-instruction source line locations. Windows modules request CodeView
  metadata; Linux modules use DWARF.
- Native Windows links now emit a sibling PDB. The CLI integration test reads
  the PDB and checks that the canonical `.bas` path and line table survived
  linking; a local `llvm-pdbutil` inspection confirmed the same for a built
  Hello World executable.
- The Linux CLI integration test runs `llvm-dwarfdump` against a native build
  and verifies the `.bas` source file and line 2 are present in the DWARF line
  table.
- A Linux GDB breakpoint at `om_runtime_fault` showed the Native caller at
  `main.bas:2` in the backtrace, confirming fault frames resolve to OMBasic
  source locations.
