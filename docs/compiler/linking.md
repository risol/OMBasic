# Native Linkers and Build Toolchains

## Standard build matrix

| Host platform | C compiler driver | Native linker | Object format and ABI |
| --- | --- | --- | --- |
| Windows x64 | `clang-cl.exe` | `lld-link.exe` | COFF, MSVC ABI |
| Linux x64 | `clang` | `ld.lld` (selected with `-fuse-ld=lld`) | ELF, Linux system ABI |

CMake and Ninja configure and build both combinations. The compiler driver
still handles platform startup files, runtime libraries, and linker arguments;
`lld-link` and `ld.lld` are the actual linkers. MinGW GCC is not part of the
Windows configuration. The Windows build uses the MSVC toolset and Windows SDK
for headers and runtime/system libraries, while `clang-cl.exe` compiles the
project and `lld-link.exe` links it. CMake rejects a different compiler family
on Windows or Linux so the configured build follows this matrix.

## Windows with clang-cl and LLD

Install LLVM 23.1.4, Ninja, CMake, Python 3, Visual Studio C++ Build Tools,
and a Windows SDK. The MSVC toolset and SDK provide headers, UCRT, and system
libraries; `cl.exe` is not the compiler selected by this configuration. CMake
selects `lld-link.exe` through `clang-cl.exe` and discovers the MSVC and SDK
library directories so native `run` and `build` commands also work from a
regular PowerShell session.

Example for LLVM installed at `F:\clang+llvm-23.1.4-x86_64-pc-windows-msvc`:

```powershell
cmake -S . -B build\windows-clang -G Ninja `
  -DCMAKE_C_COMPILER="F:/clang+llvm-23.1.4-x86_64-pc-windows-msvc/bin/clang-cl.exe" `
  -DLLVM_DIR="F:/clang+llvm-23.1.4-x86_64-pc-windows-msvc/lib/cmake/llvm" `
  -DOM_LLVM_TARGET_TRIPLE=x86_64-pc-windows-msvc
cmake --build build\windows-clang --parallel
build\windows-clang\ombasic.exe run --root examples\hello main.bas
```

The generated executable statically links the OMBasic Runtime and foundation
archives plus the MSVC C runtime libraries. LLVM is not a runtime dependency.

## Linux with Clang and LLD

Install LLVM 23.1.4 with Clang and LLD, CMake, Ninja, Python 3, and the system
C development headers and libraries. Configure with the target triple for the
host architecture:

```sh
cmake -S . -B build/linux-clang -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DLLVM_DIR="$(llvm-config --cmakedir)" \
  -DOM_LLVM_TARGET_TRIPLE=x86_64-unknown-linux-gnu
cmake --build build/linux-clang --parallel
./build/linux-clang/ombasic run --root examples/hello main.bas
```

CMake verifies that `ld.lld` is available and passes its directory to Clang, so
the driver uses that linker through `-fuse-ld=lld` even when it is not on
`PATH`. The platform's libc and startup files remain responsible for the Linux
system ABI. Set `OM_LLVM_TARGET_TRIPLE` to the actual host target when building
for a different architecture or libc ABI.
