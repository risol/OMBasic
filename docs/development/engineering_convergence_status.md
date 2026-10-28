# OMBasic 0.1 Engineering Convergence Status

Baseline commit: `a72137dde703352747c2f4dbcfc29e8f2e30877d`.

This document tracks the implementation of `OMBasic-0.1-Engineering-Convergence-Plan.md`.
The plan file is supplied as a workspace input and remains separate from the
implementation commits.

## Baseline

- Windows clang-cl / MSVC ABI Debug build succeeds with LLVM 23.1.4.
- The baseline CTest run registers 23 tests and passes 23/23 after
  `.gitattributes` pins language fixtures and IR snapshots to LF. Without the
  rule, Windows `core.autocrlf=true` caused source-offset and golden-byte
  mismatches.
- `tools/architecture_check/check.py` passes before the convergence changes.
- Baseline commit: `a72137dde703352747c2f4dbcfc29e8f2e30877d`.

## Progress

| Phase | State | Evidence |
| --- | --- | --- |
| 0. Baseline and architecture protections | Complete | Baseline passed 23/23; architecture checker rejects legacy reason keywords and build-tree archive macros. |
| 1. Language and `Actor_Down_Reason` | Complete | Removed the three reason keywords, introduced nominal qualified values, and covered positive, type-mismatch, unknown-member, and HIR identity cases; CTest passes 23/23. |
| 2. Relocatable SDK resources | Complete | Build-tree archive macros removed; explicit/env/executable-relative resource lookup, install layout, and manifest implemented. The moved-SDK test builds and runs Hello World, Actor, and Native FFI from a temporary working directory using a copied FFI fixture; it also checks CLI-over-environment precedence, `check` without Runtime archives, precise missing-archive diagnostics, and release install artifacts for repository/build paths. A second smoke installs the SDK, moves the build tree away from its configured path, then runs version/check/Hello World/Actor using only the installed SDK. Windows and Linux Release CI run this no-build-tree smoke. Windows, Linux Release, and Linux ASan relocation tests pass. |
| 3. Runtime implementation split | Complete | Removed `native_runtime.c`; split core/context/value/object, fault, GC heap/roots/trace, string/record/list/map/native pointer, Actor/mailbox/watch, scheduler, callback, and system Actor services. Added per-capability Runtime tests. Windows clang-cl and Linux clang 23 ASan/UBSan CTest pass 34/34; Linux clang 23 TSan CTest passes 34/34 on GitHub Actions, including Runtime, Actor, 2M-message stress, distribution, CLI, and SDK relocation tests. |
| 4. LLVM backend split | Complete | Replaced the 7,700-line `llvm_backend.c` with private `backend_internal.h` and focused module, type, Runtime-call, function, control-flow, GC-root, descriptor, Actor, Native FFI, callback, debug-info, and emit units. Public entry is now `om_llvm_emit(request, result)` with target triple and optimization level in the request. Windows clang-cl and Linux clang 23 ASan/UBSan full CTest pass 34/34, including LLVM output, CLI integration, debug info, FFI ABI, architecture/spec audits, Actor stress, and SDK relocation gates. |
| 5. Runtime ABI layering | Complete | Deleted the monolithic `native_runtime.h`; split generated ABI into `runtime/abi/*.h`, kept `embedding.h` small, and moved collection/object-copy/Actor-distribution internals to uninstalled Runtime headers. Added ABI v1 as the first Program descriptor field, checked it before reading later fields, and covered mismatches for both direct validation and Actor startup. All generated programs, including programs without Actors, now carry the descriptor. Windows clang-cl CTest and Linux clang 23 ASan/UBSan CTest pass 34/34; SDK relocation test compiles installed headers as C23 and verifies the old header is absent. |
| 6. GitHub Actions gates | In progress | Added Linux Release, Windows clang-cl Release, architecture/spec, install/move/use SDK, ASan/UBSan, TSan, and PR fuzz-smoke plus daily/manual extended fuzz workflows. The latest completed PR head passed all six release checks (`architecture`, `linux-release`, `windows-release`, `install-sdk`, `asan-ubsan`, `tsan`); the new SDK FFI/resource-override cases and fuzzer smoke pass locally. `master` branch protection remains pending. |
| 7. Per-compilation target model | Complete | Added explicit target description to compiler, LLVM backend, linker, CLI, and resource requests. CLI supports host-default builds and `--target`/`--sysroot`, rejects unknown or unavailable foreign targets, and supports rootless commands from the current directory. Windows and Linux CTest pass 34/34, including host, unknown, foreign-target, rootless CLI, and moved-SDK build/run coverage. |
| 8. Architecture checker | Complete | Extended dependency checks for the target model and Runtime private headers, added language-spec audits for declaration forms, built-in nominal values, and removed reason keywords, and registered both audits in CTest and `architecture-check`. Both scripts pass locally and the full Windows/Linux CTest suites pass 34/34. |
| 9. User and developer documentation | Complete | Reworked README and contributing setup for the installed SDK and LLVM/clang-cl toolchains; documented target selection, linking, resource layout, generated ABI, embedding, GC, Actors, and release gates; added a runnable Actor example and moved-SDK build/run coverage. |

## Decisions carried forward

- Native AOT is the only code-generation path.
- `EXPORT` is the canonical public declaration syntax.
- `Actor_Down_Reason` is a built-in nominal type; its values are qualified
  members, and `node_unreachable` means the remote state could not be
  confirmed.
- Native callbacks are call-scoped. The host must stop and drain callbacks
  before returning from the Native function.
- SDK resources are discovered from explicit overrides or the executable's
  installed layout; working-directory and arbitrary PATH scans are not part
  of resource lookup semantics.
