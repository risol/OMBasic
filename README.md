# OMBasic Native AOT

OMBasic is a statically typed, BASIC-style language with explicit effects and
Actor concurrency. The language rules are in
[OMBasic-Language-Standard.md](OMBasic-Language-Standard.md). The execution
architecture is being replaced with a Native AOT pipeline described in
[OMBasic-Native-AOT-Refactor-Plan.md](OMBasic-Native-AOT-Refactor-Plan.md).

The migration starts at the recorded VM baseline
`ecbbacc5fb235c4a706a9c4eeb31bc7b41dec299`. The intended compiler path is:

```text
.bas -> frontend/semantic -> typed HIR -> native MIR -> LLVM -> object -> linker -> executable
```

The VM and portable bytecode execution path have been removed. The compiler now
uses the LLVM Native AOT pipeline. The current milestone includes native scalar
execution, managed runtime values, local Actors, system Actors, and the first
direct Native FFI slice; remaining work is tracked in the implementation status
and refactor plan.

## Build on Windows with clang-cl and LLD

Install Visual Studio C++ Build Tools, a Windows SDK, CMake 4.2 or newer,
Ninja, Python 3.10 or newer, and LLVM 23.1.4. For example, if LLVM is installed
at `F:\clang+llvm-23.1.4-x86_64-pc-windows-msvc`:

```powershell
cmake -S . -B build\windows-clang -G Ninja `
  -DCMAKE_C_COMPILER="F:/clang+llvm-23.1.4-x86_64-pc-windows-msvc/bin/clang-cl.exe" `
  -DLLVM_DIR="F:/clang+llvm-23.1.4-x86_64-pc-windows-msvc/lib/cmake/llvm" `
  -DOM_LLVM_TARGET_TRIPLE=x86_64-pc-windows-msvc
cmake --build build\windows-clang --parallel
ctest --test-dir build\windows-clang --output-on-failure
```

`clang-cl.exe` compiles OMBasic and `lld-link.exe` links the Windows executable
with the MSVC ABI. MSVC C++ Build Tools and the Windows SDK provide the required
headers, UCRT, and system libraries; `cl.exe` is not the selected compiler and
MinGW GCC is not part of this build. Linux uses `clang` with `ld.lld`, selected
through `-fuse-ld=lld`. Both platforms use CMake and Ninja.

## Run Hello World

The sample is in [examples/hello/main.bas](examples/hello/main.bas). From the
repository root, run:

```powershell
build\windows-clang\ombasic.exe run --root examples\hello main.bas
```

It prints `Hello, OMBasic!`. To create a standalone executable instead:

```powershell
build/windows-clang/ombasic.exe build `
  --root examples/hello main.bas -o build/windows-clang/hello.exe
build/windows-clang/hello.exe
```

The remaining language coverage, aggregate and callback FFI, distribution,
Linux validation, and release gates are tracked in the implementation status
and refactor plan.

Architecture decisions and progress are indexed in [docs/README.md](docs/README.md).
