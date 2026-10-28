# Release gates

GitHub Actions runs these checks for pull requests targeting `master` and for
pushes to `master`:

| Workflow / job | Purpose |
|---|---|
| `linux-release` | LLVM 23 Clang Release build, CTest, architecture/spec audit, native Hello World |
| `windows-release` | LLVM 23.1.4 `clang-cl`, MSVC ABI, LLD, CTest, PDB/FFI inspection, native Hello World |
| `architecture` | Fast capability-boundary and language-specification checks |
| `install-sdk` | Install, move, then use the SDK for header smoke, version, check, Hello World and Actor build/run; repeat with the build tree removed |
| `sanitizers / asan-ubsan` | Full Linux ASan and UBSan CTest suite |
| `sanitizers / tsan` | Full Linux ThreadSanitizer CTest suite, including Actor and distribution concurrency tests |
| `fuzz / fuzz-smoke` | Build all libFuzzer targets and run a bounded smoke set on pull requests |
| `fuzz / fuzz-extended` | Run a longer per-fuzzer budget on the daily schedule or manual dispatch |

The host-build CTest suite includes native Actor stress and two-process
distribution integration tests. The Windows CLI test inspects generated PDB
debug information and links the native C and C++ FFI fixtures. The installed
SDK check moves the install prefix before invoking its compiler and generated
programs. A separate smoke test moves the exact CMake build tree out of its
configured path, then repeats version, check, Hello World, and Actor build/run
using only the installed SDK.

For `master`, branch protection should require the successful checks
`architecture`, `linux-release`, `windows-release`, `install-sdk`,
`asan-ubsan`, and `tsan`. Require the branch to be up to date before merging.
The actual GitHub check-run names are recorded after the workflows first run;
branch protection must use those exact names.

The sanitizer workflow also runs weekly and on manual dispatch. Longer fuzzing
budgets remain a separate scheduled/manual activity, while the architecture
and language lint run on every pull request.
