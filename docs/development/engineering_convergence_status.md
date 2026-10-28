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
| 3. Runtime implementation split | Not started | — |
| 4. LLVM backend split | Not started | — |
| 5. Runtime ABI layering | Not started | — |
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
