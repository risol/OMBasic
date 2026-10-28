# Target model

Each native compilation receives an explicit `Om_Target`. It describes the
LLVM triple, architecture, operating system, environment, object format, C ABI
family, pointer size, C `long` size, and endianness. The Native FFI ABI
classifier and LLVM debug-info policy consume this description rather than
re-parsing a triple independently.

## Described target families

The target module currently describes these ABI families:

| Triple family | Architecture | OS and environment | Object format | C ABI |
|---|---|---|---|---|
| `x86_64-*-windows-msvc` | x86-64 | Windows / MSVC | COFF | Win64 x64 |
| `x86_64-*-windows-gnu` | x86-64 | Windows / GNU | COFF | Win64 x64 |
| `x86_64-*-linux-gnu` | x86-64 | Linux / GNU | ELF | SysV AMD64 |
| `x86_64-*-linux-musl` | x86-64 | Linux / musl | ELF | SysV AMD64 |
| `aarch64-*-linux-*` | AArch64 | Linux | ELF | AAPCS64 |
| `aarch64-*-linux-android` | AArch64 | Android | ELF | AAPCS64 |
| `aarch64-*-freebsd` | AArch64 | FreeBSD / unspecified environment | ELF | AAPCS64 |
| `arm64-apple-darwin` | AArch64 | Darwin | Mach-O | Darwin ARM64 |

All currently described triples use 64-bit pointers and little-endian data.
The C `long` size is 4 bytes for Windows x64 and 8 bytes for the other listed
families.

## CLI selection and current support

`ombasic build --target <llvm-triple>` makes the requested target explicit.
The compiler currently emits and links only for its configured host target,
because the installed SDK carries that target's Runtime archives and the host
linker setup. An unsupported triple reports `E0701`; a described non-host
triple reports `E0702` with the missing target Runtime/sysroot reason. There
is no silent fallback to the host.

`ombasic run --target <llvm-triple>` follows the same validation and rejects a
non-host target. Running a foreign executable requires a future configured
runner or emulator.

The compiler request and LLVM backend APIs both receive the `Om_Target`
description. CMake's `OM_LLVM_TARGET_TRIPLE` is only the default host target
used to construct that description and lay out the SDK built by that
configuration. FFI classification, ELF relocation mode, and Windows CodeView
selection read target fields.

## Sysroot

`--sysroot <path>` is passed to the platform linker driver as `--sysroot=` on
Linux or `/winsysroot` on Windows. It describes platform libraries for
toolchain use; `--resource-dir` locates OMBasic resources. These options have
separate ownership. Supplying a sysroot does not enable cross-compilation or
provide a missing OMBasic Runtime archive.
