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
| 2. Relocatable SDK resources | Complete | Build-tree archive macros removed; explicit/env/executable-relative resource lookup, install layout, manifest, precise missing-archive error, and SDK move/build/run test implemented. Installed archives are stripped and Windows compiler PDB path is relative; relocation test passes. |
| 3. Runtime implementation split | In progress | Removed `native_runtime.c`; split core/context/value/object, fault, GC heap/roots/trace, string/record/list/map/native pointer, Actor/mailbox/watch, scheduler, callback, and system Actor services. Added per-capability Runtime tests. Windows clang-cl Debug CTest passes 32/32; Linux clang 23 ASan/UBSan Runtime, Actor, 2M-message stress, and distribution tests pass 12/12. Linux TSan build succeeds, but test execution is currently blocked because the container's address-space layout prevents TSan from starting; rerun in a compatible CI environment before closing the phase. |
| 4. LLVM backend split | Complete | Replaced the 7,700-line `llvm_backend.c` with private `backend_internal.h` and focused module, type, Runtime-call, function, control-flow, GC-root, descriptor, Actor, Native FFI, callback, debug-info, and emit units. Public entry is now `om_llvm_emit(request, result)` with target triple and optimization level in the request. Windows clang-cl and Linux clang 23 ASan/UBSan builds pass; full CTest passes 32/32 on both platforms, including LLVM output, CLI integration, debug info, FFI ABI, architecture, Actor stress, and SDK relocation gates. |
| 5. Runtime ABI layering | Complete | Deleted the monolithic `native_runtime.h`; split generated ABI into `runtime/abi/*.h`, kept `embedding.h` small, and moved collection/object-copy/Actor-distribution internals to uninstalled Runtime headers. Added ABI v1 as the first Program descriptor field, checked it before reading later fields, and covered mismatches for both direct validation and Actor startup. All generated programs, including programs without Actors, now carry the descriptor. Windows clang-cl CTest passes 33/33; Linux clang 23 ASan/UBSan CTest passes 33/33; SDK relocation test compiles the installed headers as C23 and verifies the old header is absent. |
| 6. GitHub Actions gates | Not started | — |
| 7. Per-compilation target model | Not started | — |
| 8. Architecture checker | Not started | — |
| 9. User and developer documentation | Not started | — |

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
