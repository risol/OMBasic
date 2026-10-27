# Contributing

## Set up a build

Use a C23-capable GCC or Clang toolchain, CMake, Ninja, Python 3, and libffi
development headers and library. Clang must be available on `PATH` when using
`ombasic bind`. See the root README for libffi installation notes.

From the repository root, configure and build with:

```powershell
cmake -S . -B build/dev -G Ninja
cmake --build build/dev --parallel
ctest --test-dir build/dev --output-on-failure
cmake --build build/dev --target architecture-check
```

The project also supports a clang-cl/MSVC frontend build. Sanitizer profiles
are separate Linux GCC/Clang builds; set `OM_SANITIZER` to
`address-undefined` or `thread`. Some container kernels require a non-PIE TSan
binary and disabled address randomization for the test process.

## Make a change

- Keep each capability behind its public headers under `include/ombasic/`.
  Follow [dependency rules](architecture/dependency_rules.md); add an ADR when
  changing a cross-capability contract or a compatibility decision.
- Use the APIs in the [C23 safety profile](development/c23_safety_profile.md).
  Raw allocation, pointer arithmetic, and platform calls stay in their owning
  implementation boundaries.
- Add tests beside the capability being changed. For invalid-input boundaries,
  include malformed, truncated, oversized, or otherwise hostile inputs where
  applicable. Keep conformance fixtures and golden output deterministic.
- For compiler changes, cover successful source and stable diagnostics. For
  bytecode or protocol changes, test both encoding and rejection paths. For
  concurrency changes, test ownership, shutdown, and stress behavior.
- Update the owning README and `docs/development/implementation_status.md` when
  behavior, limits, commands, or acceptance evidence changes.

## Run focused checks

Use CTest names to run a focused suite, for example:

```powershell
ctest --test-dir build/dev -R "lexer|parser" --output-on-failure
```

Before handing off, run the full CTest suite and `architecture-check` for the
active build. When a change affects runtime memory or concurrency, also run the
matching ASan/UBSan or TSan profile. The [testing strategy](development/testing_strategy.md)
lists the current suite boundaries.

## Documentation and review

Keep the language standard as the single authority for language rules. Put
implementation architecture and compatibility choices in the accepted ADRs;
put operational details beside the owning capability. Do not describe a
benchmark-led optimization as complete without a recorded benchmark and ADR.
