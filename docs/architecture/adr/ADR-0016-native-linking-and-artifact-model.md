# ADR-0016: Native linking and artifact model

Status: Accepted

## Context

The compiler emits LLVM-generated machine code, but end-user programs should
run independently of the compiler and LLVM installation. The native toolchain
also differs across Windows and Linux.

## Decision

- Use LLVM 23.1.4 to emit a target-native object file from verified MIR.
- On Windows, use `clang-cl` with the MSVC ABI and `lld-link`; on Linux, use
  `clang` with `ld.lld`.
- Link the program object with the static OMBasic Runtime, declared Native
  libraries, and platform system libraries to produce a standalone executable.
- Treat object and executable files as native build outputs. Do not introduce
  a bytecode or VM artifact format.
- Keep LLVM and the compiler out of the generated executable's runtime
  dependencies.
- Pass linker arguments as an argument vector, and preserve toolchain output
  needed for source-level debugging when debug information is enabled.

## Consequences

- Generated programs can run without loading LLVM or the OMBasic compiler.
- Target ABI and system library selection are build-time responsibilities.
- Object format and executable format follow the selected platform toolchain.
