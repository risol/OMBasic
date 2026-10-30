# Contributing

## Set up a build

Use CMake 4.2 or newer, Ninja, Python 3.10 or newer, and LLVM 23.x. The standard build matrix is
`clang-cl.exe` plus `lld-link.exe` on Windows, and `clang` plus `ld.lld` on
Linux. Windows also needs Visual Studio C++ Build Tools and a Windows SDK for
the MSVC ABI, UCRT, and system libraries; MinGW is not used. Linux needs the
host system's C development headers and libraries. Clang must be available on
`PATH` when using `ombasic bind`.

The shared CMake presets keep each platform, toolchain, and configuration in a stable directory. Set `LLVM_ROOT` to the LLVM 23 installation before configuring. On Windows, run from a Visual Studio Developer PowerShell:

```powershell
cmake --preset windows-clang-cl-release
cmake --build --preset windows-clang-cl-release
ctest --preset windows-clang-cl-release
```

On Linux, set `LLVM_ROOT` to the LLVM 23 prefix (for example, `/opt/llvm-23`
when the workstation toolchain is mounted in Docker), then use the matching
preset:

```sh
cmake --preset linux-clang-release
cmake --build --preset linux-clang-release
ctest --preset linux-clang-release
```

Run `python tools/architecture_check/check.py --root .` and
`python tools/spec_check/check.py --root .` for the fast architecture and
language checks. See [Native Linkers and Build Toolchains](compiler/linking.md)
for toolchain details. The Windows release preset uses clang-cl with
`lld-link`; Linux presets use Clang with `ld.lld`. Linux ASan/UBSan, TSan, and
fuzz builds use separate `RelWithDebInfo` directories and never serve as SDK
release inputs. Run `linux-clang-asan-ubsan` for the full sanitizer suite,
`linux-clang-tsan` for Runtime/Actor/Distribution concurrency tests, and
`linux-clang-fuzz` for libFuzzer targets. TSan must run on a host whose
address-space layout is compatible with its runtime; the CI gate provides that
environment. Release presets are the only presets intended for SDK packaging.

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
  HIR/MIR changes, test valid invariants and verifier rejection. For protocol
  changes, test both encoding and rejection paths. For concurrency changes,
  test ownership, shutdown, and stress behavior.
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
