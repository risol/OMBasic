# Release gates

GitHub Actions runs these checks for pull requests targeting `master` and for
pushes to `master`:

| Workflow / job | Purpose |
|---|---|
| `linux-release` | LLVM 23 Clang Release build, CTest, architecture/spec audit, native Hello World |
| `windows-release` | LLVM 23.1.4 `clang-cl`, MSVC ABI, LLD, CTest, PDB/FFI inspection, native Hello World |
| `architecture` | Fast capability-boundary checks |
| `spec` | Standard keyword table, legacy spelling, and language-contract audit |
| `conformance` | Manifest validation, 100% Rule coverage, and coverage report artifact |
| `install-sdk` | Install, move, then use the SDK for header smoke, version, check, Hello World, Actor, and Native FFI build/run; repeat with the build tree removed |
| `sanitizers / asan-ubsan` | Full Linux ASan and UBSan CTest suite |
| `sanitizers / tsan` | Full Linux ThreadSanitizer CTest suite, including Actor and distribution concurrency tests |
| `fuzz / fuzz-smoke` | Build all libFuzzer targets and run a bounded smoke set on pull requests |
| `fuzz / fuzz-extended` | Run a longer per-fuzzer budget on the daily schedule or manual dispatch |

The Linux and Windows release jobs run the full CTest suite, including the
static and runtime conformance runners. Each job stages and uploads an
installable SDK archive containing `share/ombasic/release-provenance.json`.
The provenance record names the OMBasic commit and version, LLVM, Clang, LLD,
CMake, Python, host OS, target triple, and Runtime ABI version.

The host-build CTest suite includes native Actor stress and two-process
distribution integration tests. The Windows CLI test inspects generated PDB
debug information and links the native C and C++ FFI fixtures. The installed
SDK check moves the install prefix before invoking its compiler and generated
programs, including a Native FFI program. A separate smoke test moves the exact
CMake build tree out of its configured path, then repeats version, check, Hello
World, and Actor build/run using only the installed SDK.

The intended `master` protection requires pull requests, dismissal of stale
approvals after new commits, and an up-to-date branch. Require these successful checks:
`architecture`, `spec`, `conformance`, `linux-release`, `windows-release`,
`install-sdk`, `asan-ubsan`, `tsan`, and `fuzz-smoke`. Use the exact check-run
names shown by GitHub Actions if the host includes workflow names in the check
context.

## Repository enforcement status

As of 2026-10-29, GitHub reports that `master` has no branch protection. The
available personal access token cannot write repository protection settings
(`403 Resource not accessible by personal access token`), and the signed-in
browser could not be safely targeted by the desktop automation. The required
checks therefore run in Actions, but GitHub does not yet block a merge when a
check fails.

Until a repository administrator enables the protection above, the documented
fallback is manual: maintainers must use pull requests, verify that all nine
required checks are successful for the proposed commit, and not merge with any
failed or missing check. Direct pushes are prohibited by this process, though
GitHub cannot enforce the prohibition until protection is enabled. A repository
administrator should enable branch protection before the 0.1 release candidate.

The current conformance manifest has 60 cases covering all 120 normative Rule
IDs in the checked-in language standard. The separate per-area minimums named
in the language-freeze plan are not present in this repository, so that RC
matrix gate still requires maintainer review before release.

The sanitizer workflow also runs weekly and on manual dispatch. Longer fuzzing
budgets remain a separate scheduled/manual activity, while architecture,
specification, conformance coverage, and release jobs run on every pull request.
