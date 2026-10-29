# Repository Agent Notes

## Linux LLVM toolchain

- This workstation already has the LLVM 23.1.4 Linux x64 distribution at `F:\LLVM-23.1.4-Linux-X64`.
- Do not download LLVM again for Linux validation. When using Docker Desktop, bind-mount this directory into the Linux container (for example, at `/opt/llvm-23`) and use its `bin` tools and `lib/cmake/llvm` package files.
- This directory contains Linux binaries; run them inside a Linux container rather than directly on Windows.
- Install only missing OS-level dependencies needed by a validation run; check the existing local toolchain and container image first.
