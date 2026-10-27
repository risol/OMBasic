# Implementation status

This file tracks progress against the phases and gates in `OMBasic-Compiler-Development-Plan.md`.

## Phase 0 — Specification Audit: complete

- Resolved lexical, core-language, and `.omi` grammar bindings.
- Added keyword/naming conformance fixtures and manifest validation.
- Recorded architecture and diagnostic decisions in ADR-0001 through ADR-0007.

## Phase 1 — Foundation: complete

- Implemented checked allocation, arena, vector, slices, string views, UTF-8 validation, typed IDs, and the U64 map.
- Unit coverage includes overflow, alignment, and deterministic vector sizing cases.
- Linux ASan+UBSan and both Windows toolchains passed the current Foundation suite.

## Phase 2 — Source / Module Loading: complete

- Implemented immutable source text, byte spans, line maps, configurable file-size limits, canonical source roots, and import-path validation.
- Implemented preamble import extraction, source-root-relative recursive loading, module deduplication, dependency-first ordering, and cycle-chain rendering.
- Path tests include 4,096 deterministic randomized inputs. Loader tests cover missing and malformed imports, escaped path strings, symlink rejection, exact casing, and source spans. Cycle output has a golden string assertion.
- Linux GCC ASan+UBSan, Windows GCC/Ninja, and Windows clang-cl/MSVC CTest suites passed after the Phase 2 changes.

## Phase 3 — Lexer: complete

- Added the token API, all 42 reserved keywords, identifier casing checks, integer/float scanning, string escapes, interpolation tokenization, comments, operators, and newline handling.
- Added lexical fixtures for keyword/name casing, numeric range errors, malformed exponents, string escapes, interpolation, and finite underflow; tests assert diagnostic codes and spans.
- Fuzz suites cover 4,096 arbitrary byte inputs, 2,048 string inputs, and 2,048 comment-boundary inputs. Token and diagnostic spans are checked for every case.
- Windows GCC/Ninja and clang-cl/MSVC suites passed; Linux GCC ASan+UBSan and clang-tidy analysis passed.

## Phase 4 — Parser / AST: complete

- Added an immutable syntax-only AST store with child IDs and source spans.
- Implemented recursive-descent parsing for imports, declarations, blocks, control flow, Actor state/receives, and simple statements; Pratt parsing covers literals, names, calls, member/index access, collections, interpolation, `TRY`, `SPAWN`, and `WITH`.
- Added stable E0202–E0208 parser diagnostics, synchronization at line/block boundaries, comparison-chain rejection, the direct-negative `INT64_MIN` rule, and 256-level expression/block nesting limits.
- Added normalized AST golden comparison, full grammar coverage, malformed-source recovery assertions, conformance span fixtures, and 4,096 deterministic malformed inputs.
- Windows GCC/Ninja and clang-cl/MSVC CTest suites passed after the Phase 4 changes; Linux GCC ASan+UBSan and clang-tidy gates also passed.

## Phase 5 — Name Resolution: complete

- Added shared identifier-name rules, typed `Scope_Id`, a semantic resolver API, lexical scopes, symbol IDs, and node-to-symbol/scope side tables.
- Added module import enumeration and default filename-stem qualifiers; qualified references resolve only exported declarations, with private-member diagnostics pointing at the member name.
- Implemented built-in and prelude visibility, duplicate and case-only collision detection, shadowing checks, immutable assignment errors, and `Ok(name)` / `Error(name)` arm bindings.
- Added resolver fixtures for export visibility, private members, colliding import aliases, declaration conflicts, shadowing, sibling scopes, loops, Actor state/receives, Result patterns, and unresolved names.
- Windows GCC/Ninja, Windows clang-cl/MSVC, and Linux GCC ASan+UBSan CTest suites passed (8/8 each); clang-tidy `clang-analyzer-*` passed on parser, shared name rules, module loading, and name resolution.

## Phase 6 — Type Inference: complete

- Added an interned Type Store for primitive, collection, nominal, callable, ActorRef, and Result types; all references use stable `Type_Id` values.
- Implemented level-based TypeVars, transactional structural unification, an occurs check, HM-style generalization, and fresh scheme instantiation.
- Added project-wide AST inference for primitive operations, calls, functions/actions, Record/Message constructors and fields, List/Map literals and indexing, `WITH`, `Result` constructors/patterns/`TRY`, loops, and Actor protocol references.
- Added stable E0401 type diagnostics and E0402 empty-collection diagnostics; exported Record/Message fields must have a concrete inferred interface type. There is no dynamic `Any` fallback.
- Added positive and mismatch fixtures covering polymorphic `identity`, independent call instantiations, Record/Message use, collections, Result branches, Actor spawn/send/receive, arithmetic and binding mismatches, and empty collections.
- Added Type Store tests for constrained schemes and ActorRef protocol TypeVars, alongside structural/nominal unification, occurs-check rejection, rollback, and independent polymorphic instantiations.
- Windows GCC/Ninja and clang-cl/MSVC builds and CTest suites pass (8/8 each); the semantic test binary reports 14/14.
- Recorded the algorithm and reconciled the plan's ADR numbering with the accepted diagnostics and source-path decisions in ADR-0006 through ADR-0008.

## Phase 7 — Semantic Constraints: implemented; Native pointer gate pending FFI types

- Added post-inference effect, Actor protocol, and serialization passes with queryable symbol/node summaries and protocol/type capability tables.
- Propagated Function and Action effects across project call edges; rejected hidden effects in Function, invalid WATCH/STOP context, and effectful Actor state initializers while permitting reads of earlier state.
- Collected Actor receive protocols, treated Actor_Down as universal, and checked each SEND against the receiver protocol.
- Added recursive serializability checks for primitives, ActorRef, List, Map, Record, and Message payloads. Callable and unresolved types fail closed.
- Added E0501, E0601, and E0701 diagnostics plus fixtures for indirect effect propagation, wrong protocol Message, nested non-serializable Record/List payloads, and valid nested serialization.
- Verified unknown prelude calls default to Native effects; Actor_Down is accepted by every Actor protocol.
- Windows GCC/Ninja and clang-cl/MSVC builds and CTest suites pass (8/8); the semantic test binary reports 15/15.
- The source type system has no Native pointer value kind, and the serializer fails closed for unknown type kinds. Native `CPointer` remains confined to `.omi` and cannot be constructed as an ordinary Message field; callback binding validation also rejects pointer/string callback fields.
- Recorded the pass boundaries and default-safe behavior in ADR-0009.

## Phase 8 — Bytecode: complete at the verified image/decoder boundary

- Froze the portable, field-wise little-endian OMBC v1 format in ADR-0010.
- OMBC is currently version 3; ADR-0018 adds persistent schema fingerprints
  derived from canonical module paths and structural field types.
- Added an owned bytecode builder, versioned encoder/decoder, Actor handler
  tables, Record/Message field schemas, and instruction source maps.
- Added verifier checks for tags and UTF-8 strings, schemas and constructors,
  functions and instruction ownership, operands, locals, Actor tables, source
  spans, jump targets, control-flow stack agreement, and maximum stack depth.
- Added AST and semantic lowering for literals, functions/actions, Records,
  Messages, collections, calls, short-circuit logic, IF, WHILE, numeric and
  collection FOR, SELECT including Ok/Error, TRY, Actor state, receive handlers,
  SPAWN, SEND, WATCH, and STOP.
- Added a complete 140-byte format golden, malformed image checks, source-map
  checks, 4,096 random byte arrays, 4,096 mutations of a valid image, all
  truncated prefixes, and an end-to-end fixture covering the main lowering
  paths.
- Windows GCC/Ninja, clang-cl/MSVC, Linux Clang, and Linux GCC ASan+UBSan
  bytecode suites pass (4/4). The VM repeats image verification at load time,
  and its tests reject a malformed image before retaining it.

## Phase 9 — Single-thread VM: complete

- Added explicit tagged values, immutable runtime objects, bounded VM-owned
  heap lifetime, operand stack, local slots, direct and indirect call frames,
  arithmetic, comparisons, strings, lists, maps, Record/Message constructors,
  `WITH`, indexing, Result/TRY propagation, and collection iteration.
- VM initialization verifies the image; execution returns structured runtime
  errors with bytecode positions and source spans. Integer overflow, division
  by zero, stack/frame/heap/step limits, and malformed images are covered.
- Added 11 non-Actor integration tests that compile BASIC fixtures through
  parsing, name resolution, type inference, semantic checks, bytecode emission,
  and VM execution. They cover recursion, both loop forms, indirect calls,
  records, collections, Result/error patterns, interpolation, and runtime
  failures.
- Windows GCC/Ninja, clang-cl/MSVC, Linux Clang, and Linux GCC ASan+UBSan
  complete CTest suites passed for the Phase 9 runtime baseline (10/10).

## Phase 10 — Single-thread Actor runtime: complete

- Added VM-local Actor IDs and ActorRefs, an Actor registry with separate
  state arrays, bounded FIFO mailboxes, FIFO ready scheduling, local SPAWN,
  asynchronous SEND, handler dispatch, WATCH, and Actor_Down notifications.
- The scheduler runs one message handler to completion per turn. Self-sends
  enter the mailbox, STOP unwinds the current handler and drops pending
  messages, and a Receive that propagates Error stops with reason FAILED.
- Added execution counters for spawned Actors, processed messages, and
  stopped Actors, plus configurable Actor and mailbox limits. Remote placement
  remains unsupported pending the distributed runtime.
- Added seven Actor integration checks covering deterministic scheduling,
  repeated WATCH, normal and failed Actor_Down, per-instance state isolation,
  watching an already-stopped Actor, STOP mailbox discard, and Actor/mailbox
  limits. The runtime test binary passes 18/18.
- Windows GCC/Ninja and clang-cl/MSVC CTest suites pass (10/10 each).
- Recorded scheduler and Actor ownership decisions in ADR-0012.

## Phase 11 — Actor-local GC: complete

- Added stable per-Actor heaps with an aggregate VM heap-byte limit. The root
  execution heap remains alive until VM destruction so public execution values
  keep their existing lifetime.
- Added iterative mark-and-sweep at Actor instruction safe points. Roots include
  Actor state slots, the VM operand stack, Actor call-frame locals, the current
  handler result, and mailbox messages.
- Messages are graph-copied into the receiving Actor's heap at enqueue time;
  copies preserve graph sharing and cycles. STOP and FAILED release the Actor's
  state, queued messages, and heap after watcher notifications are copied.
- Added a configurable GC threshold, deep-chain and cyclic-graph checks,
  nested List/Map/Record/Result/Iterator tracing, repeated collection, 24 seeded
  graph reachability property rounds, and Actor stress with a one-byte threshold.
- Windows GCC/Ninja and clang-cl/MSVC CTest suites pass (10/10 each); their
  runtime binary passes 21/21 checks, including the Phase 11 graph and Actor
  stress cases.
- Linux GCC ASan+UBSan through Docker passes the full CTest suite (10/10).
  GCC 13 lacks `<stdckdint.h>`, so the container used a temporary include
  overlay from its installed Clang 21 resource headers; no source shim was
  added to the project.
- `clang-analyzer-*` passes on the changed VM, heap, and runtime test files.
- Recorded heap ownership, roots, message copies, and stop cleanup in ADR-0013.

## Phase 12 — Multi-thread Scheduler: complete

- Added a bounded worker pool (default four workers, capped at 64), a locked
  FIFO ready queue, condition-variable shutdown coordination, and configurable
  instruction quanta (default 256 bytecode instructions).
- Workers keep independent VM stacks and call frames. The scheduler marks an
  Actor running while holding its mutex and queues each Actor at most once;
  saved Actor contexts are resumed after a quantum or moved to another worker.
- Messages are copied into mailbox-owned transit heaps, then copied into the
  recipient Actor heap when dispatched. No worker writes into another running
  Actor's heap.
- Added a one-worker, one-instruction fairness regression and a concurrent
  two-pump stress test that processes 2,000,000 messages, checks both exact
  per-Actor counts, and observes at least two Actors running at once.
- Windows GCC/Ninja and clang-cl/MSVC full CTest suites pass (10/10 each); the
  runtime binary passes 23/23 checks and passed five consecutive repeat runs.
- Linux GCC ThreadSanitizer passes the runtime suite, including the two-million
  message stress test. The container required a non-PIE TSan executable to
  avoid an address-mapping failure before test startup.
- Recorded worker ownership, mailbox transit heaps, and quantum scheduling in
  ADR-0014.

## Phase 13 — System Actors: complete

- Added built-in `Console`, `File_System`, and `Timer` ActorRefs and the
  `LOAD_SYSTEM_ACTOR` opcode. OMBC was advanced to v2 for the new instruction.
- Added system request/completion schemas and compiler support for
  `Console_Write`, `File_Read`, `File_Write`, and `Timer_After`; semantic
  analysis checks their field types and Actor protocols.
- Console and file operations run in the bounded blocking pool. Requests copy
  their UTF-8 strings into native buffers; workers do not retain Actor-heap
  pointers. I/O status is wrapped in `Result` and returned as a completion
  Message through the ordinary mailbox path.
- Added a separate ordered monotonic Timer queue. It sends the requested
  Message to its recipient on expiry. The Actor scheduler waits for external
  work and is notified when a completion is enqueued.
- VM shutdown drains completed work on success. On scheduler failure it cancels
  queued blocking requests, waits for active native calls, cancels pending
  timers, and joins the service threads before releasing VM state.
- Worker-local VM stacks and call-frame buffers are released after workers
  join; Timer service test contexts are destroyed after completion/cancellation.
- Added unit coverage for blocking-pool concurrency, bounds and cancellation;
  Timer ordering, explicit cancellation and shutdown cancellation; and a
  compiled OMBasic integration fixture that writes then reads UTF-8 text,
  receives Console and Timer messages, and exits after all five messages are
  processed.
- Windows GCC/Ninja and clang-cl CTest suites pass (12/12); the runtime binary
  passes 25/25 checks. Linux GCC ASan/UBSan passes the `runtime_vm`,
  `blocking_pool`, and `timer_service` checks (3/3).
- Linux TSan could not start `runtime_vm` in the current container because of
  `unexpected memory mapping`; it failed before executing tests and produced no
  race report. Recorded the system actor execution and shutdown policy in
  ADR-0015.

## Phase 14 — Native FFI: implemented

- Added an independent `.omi` tokenizer/parser and metadata model for Native
  functions, callback signatures, Native Records, effects, blocking status,
  symbols, and ownership. Invalid pointer ownership and callback bindings
  produce stable source-spanned diagnostics.
- Added host C ABI size/alignment mapping and default struct layout calculation
  for supported scalar/pointer fields.
- Added UTF-8 dynamic library loading and symbol lookup for Windows and POSIX,
  bounded CString conversion, and release through an `.omi`-declared Owned
  pointer destructor.
- Added a libffi-backed typed call engine that binds parsed `.omi` declarations
  to library symbols. It marshals supported integer and floating-point types,
  bounded UTF-8 strings, pointer values, and Owned return values; invalid scalar
  ranges and malformed strings fail before entering native code.
- Added `om_vm_native_callback_post` as a thread-safe Runtime ingress. It copies
  primitive callback fields into a transit-heap Message, applies normal mailbox
  bounds, and schedules delivery through the Actor scheduler. The VM now creates
  temporary libffi callback closures, validates callback-to-Message bindings,
  and never runs Actor handlers on the callback thread.
- Resolver configuration can provide parsed `.omi` modules. Name resolution
  imports supported Native function declarations; type inference imports their
  ABI types and effects; bytecode emission creates fixed-arity external
  declarations; `om_compiler_bind_native_module` checks callback Message types,
  then asks the Runtime to own their call bindings. The Runtime public header no
  longer depends on semantic types.
- Added `Om_Omi_Project` to load an explicit list of canonical `.omi` paths
  under the source root using the same case and reparse-point protections as
  `.bas` loading. The Native dispatch integration fixture now reads its metadata
  from disk and passes the loaded module into the compiler pipeline.
- Actor calls marked `BLOCKING` execute in the bounded blocking pool. The caller
  Actor saves its VM context and releases the scheduler worker; task completion
  stores the result and schedules the Actor to resume. Root calls remain
  synchronous because they do not occupy an Actor worker.
- Added end-to-end BASIC-to-`.omi` typed dispatch and a one-worker Actor test
  proving a second Actor can run while a blocking Native call waits. Existing
  tests cover CString and Owned-return handling, the `extern "C"` C++ wrapper,
  and a callback invoked from a foreign thread into a running Actor mailbox.
- Callback closures are scoped to the Native call; the Native function must
  finish callback threads before returning. Callback payloads are limited to
  supported scalar fields, copied into Runtime-owned storage.
- Native function parameters and return values can now reference a Native
  Record declared in the same `.omi` block. The FFI binder builds libffi struct
  types from the record fields and checks libffi's size/alignment against the
  host C layout. Arguments are copied into aligned temporary storage; returned
  records are owned by `Om_Native_Call_Result` until destruction. The C ABI
  roundtrip gate covers a padded `{ int32_t, double }` argument and return.
- clang-cl and GCC/Ninja full CTest suites both pass (14/14), including the
  aggregate FFI roundtrip.
- This aggregate support is at the FFI binding API. Resolver and VM imports
  still expose only scalar Native values, so Native Record functions are not
  callable from BASIC source yet. Nested records and callback Records are not
  supported. Pointer fields copy their pointer values only; pointee ownership
  remains the caller/wrapper's responsibility. Applications must list `.omi`
  paths in project configuration; the loader does not scan or guess metadata
  dependencies.

## Phase 15 — Distribution: implemented

- Added OS-random 128-bit Node identity generation, a version-3 big-endian
  frame codec with a bounded length prefix, stable 64-bit Message schema
  fingerprints, a bounded field-wise Message serializer, and blocking TCP
  listener/connect/accept/send/receive APIs.
- Sequential sends on one connection preserve stream order. The transport
  never retries; malformed or oversized frames and partial writes close the
  connection. Added localhost ordering, bounds, fingerprint, node identity,
  nested-value roundtrip, and 5,000-case malformed-frame and payload checks.
- Added a symmetric HELLO/HELLO_ACK handshake layered over TCP. It checks the
  protocol version, distinct node identities, an optional expected peer, and
  matching program and interface fingerprints. Incompatible peers close the
  connection; localhost tests cover a compatible pair and fingerprint rejection.
- The two-node test now sends a Message frame after a successful handshake.
  Serializer limits include a total value budget, so many nested containers
  cannot multiply a bounded payload into unbounded decoded allocations.
- The schema-fingerprint API accepts 64-bit structural type IDs, and OMBC v3
  stores program/interface fingerprints in its header and each emitted
  Record/Message fingerprint for VM and serializer use.
- clang-cl and GCC/Ninja full CTest suites pass (14/14).
- Recorded the wire and transport decisions in ADR-0017.
- Each initialized VM now owns a fresh 128-bit Node identity and exposes a
  handshake descriptor populated from its loaded image's program/interface
  fingerprints. The transport handshake can return the peer descriptor after
  validating it.
- Added VM-owned peer attachment with a receiver thread, recursive VM value
  serialization/deserialization, remote ActorRef handles, schema-checked
  remote `SEND`, remote `WATCH`, `Actor_Down` delivery when a watched Actor
  stops, and `NODE_UNREACHABLE` delivery when a peer disconnects. Receiver
  threads validate network frames and enqueue Messages; they do not execute
  Actor handlers.
- Added a two-VM localhost Actor test covering remote List/Map payload delivery,
  remote Watch/normal stop notification, typed `Actor_Id`/`Down_Reason` values,
  peer-disconnect notification, remote spawn, and remote actor-limit rejection.
  GCC/Ninja and clang-cl full CTest suites pass (14/14).
- A remote spawn request waits for an acknowledgement that the peer allocated
  the Actor. The peer queues its initializer and runs it before pending mailbox
  messages on the next `om_vm_execute`. The host must keep pumping VM execution
  for queued remote work. Automatic retry, reconnect deduplication, and peer
  authentication remain future work.

## Phase 16 — Tooling: implemented

- Added deterministic `ombasic fmt`, including syntax validation, normalized
  indentation/spacing/blank lines/LF endings, and preservation of regular and
  documentation comments. Formatter idempotence is covered by a regression
  test.
- Added `ombasic check` and structured JSON diagnostics for lexer, parser,
  name-resolution, type-inference, and semantic errors. Ranges carry stable
  codes, file paths, one-based lines, and one-based UTF-8 byte columns.
- Added `ombasic run` to compile and execute an entry function and `ombasic
  build` to write a portable OMBC bytecode image. The run command prints scalar
  return values; runtime errors include source locations when available.
- Added the in-memory `Om_Interface_Cache` API. It records exported symbols,
  structural interface hashes, effects, serializability, and exported Actor
  protocol pairs while borrowing the compiler results that own its names and
  types.
- Added an early stdio LSP server for diagnostics, hover, and type inlay hints.
  It routes source through the existing compiler pipeline and refreshes types
  after full or ranged document changes.
- Added `ombasic bind`, which reads Clang's JSON AST and emits scalar and
  single-pointer OMI signatures. Unsupported declarations are skipped. The
  generated defaults for effect, blocking, and pointer ownership are explicit
  placeholders that must be reviewed; `ombasic check-omi` validates output.
- `tooling_formatter` and `tooling_cli` cover formatter idempotence, diagnostic
  JSON, run/build output, bind generation/validation, and LSP hover/inlay
  against edited source.

## Reference implementation release-gate review — 2026-10-28

- Windows GCC/Ninja and clang-cl/MSVC both build successfully and pass the full
  CTest suite (16/16). The architecture checker is included and passes.
- Linux GCC ASan/UBSan builds the current source and passes the full CTest
  suite (16/16), including the VM concurrency stress test.
- Linux GCC TSan passes the Runtime VM suite (33/33 checks), including the
  two-million-message Actor stress test. In this Docker environment it needs a
  non-PIE executable, address randomization disabled with `setarch -R`, and an
  unconfined seccomp profile so the test process can set its address layout.
- Deterministic fuzz-style tests cover 4,096 lexer byte inputs, 4,096 malformed
  parser inputs, bytecode random images and mutations, GC object graphs, and
  5,000 malformed distribution frames/payloads; no crashes are known.
- The architecture overview, contribution guide, bytecode format, Native ABI,
  wire protocol, and C23 safety profile are indexed from `docs/README.md`.
- Phase 17 optimizations remain deferred: that phase is optional and requires
  benchmark evidence and an ADR before introducing optimization work.
