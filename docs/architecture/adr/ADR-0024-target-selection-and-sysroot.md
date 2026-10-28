# ADR-0024: Make target and sysroot per-compilation inputs

- Status: Accepted
- Date: 2026-10-29

## Context

The LLVM target triple is currently fixed when the compiler is built, while
Runtime archive selection and native ABI policy are also host-oriented.

## Decision

Each compilation request carries a target descriptor and optional sysroot.
The descriptor contains the target triple, architecture, OS, environment,
object format, C ABI family, pointer size, C `long` size, and endianness. CMake's
configured target is only the default. Runtime archives are selected from the
SDK by triple. Unsupported target/runtime/sysroot combinations receive a
structured diagnostic. Running a non-host target is rejected unless a runner
is explicitly supported.

## Consequences

- Backend and linker APIs do not infer ABI from a hardcoded build-time triple.
- The first release can support only host execution while exposing the
  per-compilation target model.
