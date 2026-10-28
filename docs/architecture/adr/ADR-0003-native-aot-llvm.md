# ADR-0003: Native AOT with LLVM

- **Status:** Accepted
- **Date:** 2026-10-28
- **Baseline:** `ecbbacc5fb235c4a706a9c4eeb31bc7b41dec299`

## Context

The reference compiler has a complete portable-bytecode and VM execution
route. That route couples the VM to Actor scheduling, heap values, Native
calls, and distribution. Maintaining it alongside a native backend would keep
the old execution model as a permanent fallback and duplicate semantics.

## Decision

Native AOT is the sole execution backend. The migration removes VM, bytecode,
and OMBC artifacts without compatibility shims. The frontend and semantic
model lower to typed HIR, specialization, and native MIR. Only the LLVM backend
depends on LLVM C headers and emits native object files. LLD or the configured
platform linker produces a standalone executable.

Generated native code calls a versioned internal Runtime ABI. Runtime does not
interpret language instructions and does not depend on compiler IR or LLVM.
The compiler source remains C23 and uses LLVM's C API.

## Runtime and value model

- Int is checked signed 64-bit, Float is IEEE-754 double, and Bool is an SSA
  `i1` lowered to a byte in memory when needed.
- Strings, Records, Messages, Lists, and Maps are non-moving heap objects.
  Their first-version object header carries a type descriptor, GC link, and
  flags. Concrete Record layouts are compiler-owned; Message values are
  immutable.
- Every generated OMBasic function receives an `Om_Execution_Context *`
  hidden parameter. It exposes Runtime, current Actor/heap, shadow-root chain,
  and a fault boundary; it contains no instruction pointer, operand stack,
  bytecode image, or VM frame.
- Generated functions register managed parameters and locals in a shadow-root
  frame. Actor state, the current Message, and Runtime temporaries are roots.
  First-version collection is non-moving mark-and-sweep.
- An Actor handler has the native ABI
  `Om_Handler_Result (*)(Om_Execution_Context *, void *, const void *)` and
  executes one Message to completion on an OS worker. Runtime does not save or
  migrate native continuations.
- Static type, Message, Actor, and Program descriptors are linked into the
  executable. They describe size/alignment, tracing/copying, schema identity,
  handlers, source metadata, and entry Action as applicable.

## Target and linker policy

Development uses LLVM 23.1.x and currently installed LLVM 23.1.4. The first
backend supports the host target and uses LLVM's C API to emit `.o`/`.obj`.
The linker receives an argument vector, not a shell command string. The first
runtime link model is static, so installed programs do not require LLVM or a
separately installed OMBasic Runtime.

## Consequences

The old VM execution APIs, tests, and ADRs are removed from the main tree. A
period without `build` or `run` is expected while the native pipeline is built.
The compiler must own HIR/MIR semantics and verifier invariants before LLVM
code generation is enabled. Runtime behavior is rebuilt against explicit
descriptors and ABI contracts.

## References

- [Native Runtime ABI](../../runtime/runtime_abi.md)
- [HIR contract](../../compiler/hir.md)
- [MIR contract](../../compiler/mir.md)
